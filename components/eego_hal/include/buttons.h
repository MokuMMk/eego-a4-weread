// EEGO A4 按键管理 — 三键异步输入队列
// 纯 ESP-IDF，FreeRTOS 后台轮询任务
#pragma once
#include <stdint.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace eego {

enum class ButtonId { Up, Down, Power };
enum class ButtonAction { ShortPress, LongPress };

struct ButtonEvent {
    ButtonId id;
    ButtonAction action;
};

class ButtonManager {
public:
    static ButtonManager& instance();

    esp_err_t begin();  // 初始化 GPIO + 启动后台轮询任务
    void end();

    bool poll(ButtonEvent* event, uint32_t timeoutMs = 0);  // 从队列取事件

private:
    ButtonManager() = default;
    static void pollTask(void* arg);
    bool readDebounced(int pin, bool activeHigh);
    QueueHandle_t queue_ = nullptr;
    TaskHandle_t task_ = nullptr;
    bool running_ = false;
};

}  // namespace eego
