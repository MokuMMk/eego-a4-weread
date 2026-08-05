// sleep_manager.cpp — 休眠/唤醒管理实现（Task 16）
//
// 浅睡：CPU 不停止，关 WiFi + 显示封面 → 双击唤醒 → 恢复 UI → 后台重连 WiFi
// 深睡：esp_deep_sleep_start() + ESP32 内部定时器唤醒 + GPIO4 锁存保持
//
// 注意：PCF8563 RTC 的 INT 引脚在本板未连接到 ESP32 可唤醒 GPIO，
// 深睡定时唤醒使用 ESP32 内部 esp_sleep_enable_timerwakeup()。
// PCF8563 仍用于时间持久化（深睡前后读写时间）。
#include "sleep_manager.h"
#include "eego_pins.h"
#include "gslx680_touch.h"
#include "uc8279c_display.h"
#include "wifi_manager.h"
#include "weread_app.h"
#include "font_manager.h"
#include "portrait_canvas.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

static const char* TAG = "eego.sleep";

namespace eego {

// ---- 常量 ----
static constexpr uint32_t DEEP_SLEEP_MIN_S = 3;
static constexpr uint32_t DEEP_SLEEP_MAX_S = 60;
static constexpr uint32_t DOUBLE_TAP_WINDOW_MS = 700;   // 双击窗口
static constexpr uint32_t DOUBLE_TAP_DEDUP_MS = 50;     // 同一按下事件去重阈值
static constexpr uint64_t LIGHT_SLEEP_TIMEOUT_MS = 24ULL * 60 * 60 * 1000;  // 24 小时

SleepManager& SleepManager::instance() {
    static SleepManager inst;
    return inst;
}

void SleepManager::init() {
    esp_err_t err = rtc_.begin();
    rtcReady_ = (err == ESP_OK);
    if (rtcReady_) {
        RtcTime t = rtc_.readTime();
        if (t.valid) {
            ESP_LOGI(TAG, "RTC time: %04u-%02u-%02u %02u:%02u:%02u",
                     t.year, t.month, t.day, t.hour, t.minute, t.second);
        } else {
            ESP_LOGW(TAG, "RTC time invalid (VL flag set), cleared");
        }
    } else {
        ESP_LOGW(TAG, "PCF8563 not detected, deep sleep will use ESP32 timer");
    }
}

void SleepManager::setDeepSleepTimer(uint32_t seconds) {
    if (seconds != 0 && seconds < DEEP_SLEEP_MIN_S) seconds = DEEP_SLEEP_MIN_S;
    if (seconds > DEEP_SLEEP_MAX_S) seconds = DEEP_SLEEP_MAX_S;
    deepSleepTimer_ = seconds;
    ESP_LOGI(TAG, "Deep sleep timer set to %us", deepSleepTimer_);
}

bool SleepManager::wokeFromDeepSleep() {
    return esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER;
}

esp_sleep_wakeup_cause_t SleepManager::wakeupCause() {
    return esp_sleep_get_wakeup_cause();
}

// ================================================================
// 浅睡
// ================================================================

void SleepManager::enterLightSleep() {
    ESP_LOGI(TAG, "进入浅睡");

    WereadApp& app = WereadApp::instance();

    // 1. 保存 cookie（防重启后旧 cookie 失效）
    if (WR.cookiesDirty()) {
        WR.saveCookiesToConfig();
        WR.clearCookiesDirty();
    }

    // 2. 显示当前书封面（WereadApp 负责 UI 操作）
    app.displaySleepCover();

    // 3. 等波形真正刷完
    vTaskDelay(pdMS_TO_TICKS(2000));

    // 4. EPD 断电（墨水屏图像永久保持）
    // 由 WereadApp::displaySleepCover 内部处理 display->deepSleep()

    // 5. 断 WiFi 省电
    ESP_LOGI(TAG, "断开 WiFi（省电）");
    WifiManager::instance().powerOff();

    ESP_LOGI(TAG, "浅睡中，等待电源键唤醒（24h 后自动转深睡）");

    // 6. 等待电源键唤醒，24 小时超时则转深睡
    const int64_t sleepStartMs = esp_timer_get_time() / 1000;
    while (true) {
        // 仅响应电源键，忽略触控（防误触）
        ButtonEvent bevt;
        if (ButtonManager::instance().poll(&bevt, 0)) {
            if (bevt.id == ButtonId::Power) {
                ESP_LOGI(TAG, "电源键唤醒");
                break;
            }
        }
        // 24 小时无人唤醒 → 转入深睡（电源键唤醒）
        if (esp_timer_get_time() / 1000 - sleepStartMs > (int64_t)LIGHT_SLEEP_TIMEOUT_MS) {
            ESP_LOGI(TAG, "浅睡满 24 小时，转入深睡");
            enterDeepSleepOnButton();
            return;  // 不会返回（深睡不返回）
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 7. 唤醒点（程序原地恢复，不重启）
    ESP_LOGI(TAG, "已唤醒（电源键）");

    // 8. 先恢复 UI 显示（WereadApp 负责恢复 EPD 电源 + 重绘当前页）
    app.onWakeFromSleep();

    // 9. 后台静默重连 WiFi（非阻塞，不弹 UI）
    if (!WifiManager::instance().isConnected()) {
        ESP_LOGI(TAG, "后台静默重连 WiFi...");
        WifiManager::instance().reconnectBackground();
    }
}

bool SleepManager::detectDoubleTap() {
    Gslx680Touch* touch = WereadApp::instance().touch();
    if (!touch) {
        // 无触控驱动，等待电源键
        vTaskDelay(pdMS_TO_TICKS(500));
        return false;
    }

    // 等待第一击（阻塞轮询，每 50ms 检查一次）
    int64_t firstPressMs = 0;
    while (true) {
        TouchEvent evt = touch->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            firstPressMs = esp_timer_get_time() / 1000;
            break;
        }
        // 也检测电源键唤醒（简化：电源键直接唤醒）
        ButtonEvent bevt;
        if (ButtonManager::instance().poll(&bevt, 0)) {
            if (bevt.id == ButtonId::Power) {
                ESP_LOGI(TAG, "电源键唤醒");
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    // 丢弃第一击的后续事件（同一按下的多个上报，时间差 <50ms）
    int64_t deadline = firstPressMs + DOUBLE_TAP_WINDOW_MS;
    while (esp_timer_get_time() / 1000 - firstPressMs < DOUBLE_TAP_DEDUP_MS) {
        touch->poll();  // 丢弃
        vTaskDelay(pdMS_TO_TICKS(10));
    }

    // 700ms 窗口内等第二击
    while (esp_timer_get_time() / 1000 < deadline) {
        TouchEvent evt = touch->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            int64_t now = esp_timer_get_time() / 1000;
            if (now - firstPressMs > DOUBLE_TAP_DEDUP_MS) {
                return true;  // 有效双击
            }
            // 否则是同一按下的连续上报，丢弃
        }
        // 电源键也唤醒
        ButtonEvent bevt;
        if (ButtonManager::instance().poll(&bevt, 0)) {
            if (bevt.id == ButtonId::Power) {
                ESP_LOGI(TAG, "电源键唤醒");
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    return false;  // 超时未检测到双击（单击）
}

// ================================================================
// 深睡
// ================================================================

void SleepManager::enterDeepSleep(uint32_t seconds) {
    if (seconds < DEEP_SLEEP_MIN_S) seconds = DEEP_SLEEP_MIN_S;
    if (seconds > DEEP_SLEEP_MAX_S) seconds = DEEP_SLEEP_MAX_S;

    ESP_LOGI(TAG, "进入深睡 %us 后 RTC/定时器唤醒", seconds);

    // 1. 保存 cookie
    if (WR.cookiesDirty()) {
        WR.saveCookiesToConfig();
        WR.clearCookiesDirty();
    }

    // 2. 断 WiFi
    WifiManager::instance().powerOff();

    // 3. 显示深睡提示（简化：不画封面，直接清屏提示）
    // 由调用方在调用前处理 UI

    // 4. GPIO4 锁存保持（深睡期间电源不断）
    holdGpio4ForDeepSleep();

    // 5. 配置定时唤醒（ESP32 内部定时器，us 精度）
    esp_err_t err = esp_sleep_enable_timer_wakeup((uint64_t)seconds * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_sleep_enable_timerwakeup failed: %s", esp_err_to_name(err));
    }

    // 6. 进入深睡（不会返回）
    ESP_LOGI(TAG, "esp_deep_sleep_start()");
    esp_deep_sleep_start();

    // 不会执行到这里
}

void SleepManager::holdGpio4ForDeepSleep() {
    // GPIO4 电源锁存：在 PowerManager::holdPower() 中已配置
    // 这里确保深睡期间 hold 生效
    gpio_hold_en(static_cast<gpio_num_t>(POWER_LATCH));
    gpio_deep_sleep_hold_en();
    ESP_LOGI(TAG, "GPIO%d hold enabled for deep sleep", POWER_LATCH);
}

// ================================================================
// 深睡等待电源键唤醒（浅睡 24h 超时后调用）
// ================================================================

void SleepManager::enterDeepSleepOnButton() {
    ESP_LOGI(TAG, "进入深睡（电源键 GPIO%d 唤醒）", BUTTON_POWER);

    // 1. 保存 cookie
    if (WR.cookiesDirty()) {
        WR.saveCookiesToConfig();
        WR.clearCookiesDirty();
    }

    // 2. 断 WiFi（已在浅睡时断开，这里确保）
    WifiManager::instance().powerOff();

    // 3. GPIO4 锁存保持（深睡期间电源不断）
    holdGpio4ForDeepSleep();

    // 4. 配置电源键（GPIO8, active-high）EXT0 唤醒
    //    ESP32-S3 的 GPIO8 是 RTC_GPIO8，支持 EXT0 唤醒
    //    active-high: 按下为高电平，唤醒电平=1
    esp_err_t err = esp_sleep_enable_ext0_wakeup(
        static_cast<gpio_num_t>(BUTTON_POWER), 1);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "EXT0 唤醒配置失败: %s", esp_err_to_name(err));
        // 退路：60s 定时器唤醒
        esp_sleep_enable_timer_wakeup(60ULL * 1000000ULL);
    }

    // 5. 进入深睡（不会返回）
    ESP_LOGI(TAG, "esp_deep_sleep_start() — 按电源键唤醒");
    esp_deep_sleep_start();
}

}  // namespace eego
