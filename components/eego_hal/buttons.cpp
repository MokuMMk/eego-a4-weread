// EEGO A4 按键管理实现
// 三键: GPIO5(Up, active-low), GPIO7(Down, active-low), GPIO8(Power, active-high)
// 5ms 去抖 + 短按/长按检测 + FreeRTOS 异步轮询任务
#include "buttons.h"
#include "eego_pins.h"

#include <esp_log.h>
#include <esp_timer.h>
#include <driver/gpio.h>

static const char* BTN_TAG = "eego_btn";

// 长按阈值（微秒）
static constexpr int64_t LONG_PRESS_US = 700 * 1000LL;
// 轮询周期
static constexpr uint32_t POLL_PERIOD_MS = 15;
// 去抖延时
static constexpr uint32_t DEBOUNCE_MS = 5;
// 队列深度
static constexpr uint32_t QUEUE_LEN = 32;
// 任务栈大小
static constexpr uint32_t TASK_STACK = 4096;
// 任务优先级（低于显示刷新）
static constexpr UBaseType_t TASK_PRIORITY = 2;

namespace eego {

// 按键配置表
struct BtnCfg {
    ButtonId id;
    int pin;
    bool activeHigh;
};

static const BtnCfg kButtons[] = {
    {ButtonId::Up,    BUTTON_UP,    false},  // active-low
    {ButtonId::Down,  BUTTON_DOWN,  false},  // active-low
    {ButtonId::Power, BUTTON_POWER, true},   // active-high
};
static constexpr size_t kButtonCount = sizeof(kButtons) / sizeof(kButtons[0]);

ButtonManager& ButtonManager::instance() {
    static ButtonManager inst;
    return inst;
}

bool ButtonManager::readDebounced(int pin, bool activeHigh) {
    // 读取 GPIO 状态后等 5ms 再读一次，两次一致才确认
    int level1 = gpio_get_level(static_cast<gpio_num_t>(pin));
    vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));
    int level2 = gpio_get_level(static_cast<gpio_num_t>(pin));
    if (level1 != level2) return false;
    return activeHigh ? (level1 == 1) : (level1 == 0);
}

esp_err_t ButtonManager::begin() {
    if (running_) return ESP_OK;  // 已启动

    // 配置 Up/Down 引脚：active-low → INPUT_PULLUP
    gpio_config_t pullupConf;
    pullupConf.pin_bit_mask = (1ULL << BUTTON_UP) | (1ULL << BUTTON_DOWN);
    pullupConf.mode = GPIO_MODE_INPUT;
    pullupConf.pull_up_en = GPIO_PULLUP_ENABLE;
    pullupConf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    pullupConf.intr_type = GPIO_INTR_DISABLE;
    esp_err_t err = gpio_config(&pullupConf);
    if (err != ESP_OK) {
        ESP_LOGE(BTN_TAG, "Up/Down GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 配置 Power 引脚：active-high → INPUT_PULLDOWN
    gpio_config_t pulldownConf;
    pulldownConf.pin_bit_mask = (1ULL << BUTTON_POWER);
    pulldownConf.mode = GPIO_MODE_INPUT;
    pulldownConf.pull_up_en = GPIO_PULLUP_DISABLE;
    pulldownConf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    pulldownConf.intr_type = GPIO_INTR_DISABLE;
    err = gpio_config(&pulldownConf);
    if (err != ESP_OK) {
        ESP_LOGE(BTN_TAG, "Power GPIO config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 创建事件队列
    queue_ = xQueueCreate(QUEUE_LEN, sizeof(ButtonEvent));
    if (!queue_) {
        ESP_LOGE(BTN_TAG, "Queue creation failed");
        return ESP_ERR_NO_MEM;
    }

    // 启动后台轮询任务
    running_ = true;
    BaseType_t ret = xTaskCreate(pollTask, "eego_btn", TASK_STACK, this,
                                  TASK_PRIORITY, &task_);
    if (ret != pdPASS) {
        ESP_LOGE(BTN_TAG, "Task creation failed");
        running_ = false;
        vQueueDelete(queue_);
        queue_ = nullptr;
        return ESP_FAIL;
    }

    ESP_LOGI(BTN_TAG, "Button manager started (3 buttons, %ums poll)", POLL_PERIOD_MS);
    return ESP_OK;
}

void ButtonManager::end() {
    running_ = false;
    if (task_) {
        TaskHandle_t t = task_;
        task_ = nullptr;
        // 从外部删除任务（FreeRTOS 会释放栈和 TCB）
        vTaskDelete(t);
    }
    if (queue_) {
        vQueueDelete(queue_);
        queue_ = nullptr;
    }
}

bool ButtonManager::poll(ButtonEvent* event, uint32_t timeoutMs) {
    if (!queue_ || !event) return false;
    return xQueueReceive(queue_, event, pdMS_TO_TICKS(timeoutMs)) == pdTRUE;
}

void ButtonManager::pollTask(void* arg) {
    ButtonManager* self = static_cast<ButtonManager*>(arg);

    // 每个按键的运行时状态
    struct BtnState {
        bool pressed;        // 当前去抖确认的按下状态
        int64_t pressStart;  // 按下起始时间（微秒）
        bool longSent;       // 长按事件已发送
    };
    BtnState states[kButtonCount];
    for (size_t i = 0; i < kButtonCount; i++) {
        states[i] = {false, 0, false};
    }

    while (self->running_) {
        // 第一轮采样所有按键
        bool sample1[kButtonCount];
        for (size_t i = 0; i < kButtonCount; i++) {
            int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[i].pin));
            sample1[i] = kButtons[i].activeHigh ? (level == 1) : (level == 0);
        }

        // 5ms 去抖等待
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        // 第二轮采样
        bool sample2[kButtonCount];
        for (size_t i = 0; i < kButtonCount; i++) {
            int level = gpio_get_level(static_cast<gpio_num_t>(kButtons[i].pin));
            sample2[i] = kButtons[i].activeHigh ? (level == 1) : (level == 0);
        }

        int64_t now = esp_timer_get_time();

        for (size_t i = 0; i < kButtonCount; i++) {
            // 两次采样不一致 → 跳过（去抖未通过）
            if (sample1[i] != sample2[i]) continue;

            bool nowPressed = sample2[i];

            // 按下边沿
            if (nowPressed && !states[i].pressed) {
                states[i].pressed = true;
                states[i].pressStart = now;
                states[i].longSent = false;
            }
            // 释放边沿
            else if (!nowPressed && states[i].pressed) {
                states[i].pressed = false;
                // 若长按未发送 → 短按事件
                if (!states[i].longSent) {
                    ButtonEvent evt = {kButtons[i].id, ButtonAction::ShortPress};
                    xQueueSend(self->queue_, &evt, 0);
                }
            }

            // 按住期间检测长按阈值
            if (states[i].pressed && !states[i].longSent) {
                if (now - states[i].pressStart >= LONG_PRESS_US) {
                    states[i].longSent = true;
                    ButtonEvent evt = {kButtons[i].id, ButtonAction::LongPress};
                    xQueueSend(self->queue_, &evt, 0);
                }
            }
        }

        // 轮询周期延时
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
    }

    self->task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace eego
