// sleep_manager.h — 休眠/唤醒管理（Task 16）
//
// 功能：
//   - 5 分钟闲置浅睡（关 WiFi + 显示封面，CPU 不停止）
//   - 双击触控唤醒（700ms 窗口 + 50ms 时间戳去重）
//   - 唤醒后后台静默重连 WiFi（非阻塞）
//   - state.json 持久化 + 启动恢复（由 WereadApp::saveProgress/tryRestoreReading 负责）
//   - 定时深睡 3-60s（PCF8563 RTC 或 ESP32 内部定时器唤醒，GPIO4 锁存保持）
//
// 纯 ESP-IDF，无 Arduino 依赖。代码在 namespace eego 中。
#pragma once

#include "esp_err.h"
#include "esp_sleep.h"
#include "pcf8563_rtc.h"
#include <cstdint>

namespace eego {

// 前向声明（避免循环 include）
class Gslx680Touch;
class Uc8279cDisplay;

class SleepManager {
public:
    static SleepManager& instance();

    // 初始化 RTC（可在 run() 早期调用）
    void init();

    // ---- 浅睡 ----
    // 进入浅睡：显示封面 → 断 WiFi → 等待双击唤醒 → 恢复 UI → 后台重连 WiFi
    // 24 小时无人唤醒则自动转入深睡（电源键唤醒）
    // 调用前调用方应已保存 state.json（WereadApp::saveProgress 已做）
    void enterLightSleep();

    // ---- 深睡 ----
    // 进入深睡：配置定时唤醒 → GPIO4 锁存保持 → esp_deep_sleep_start()
    // seconds: 3-60s（超出范围自动 clamp）
    void enterDeepSleep(uint32_t seconds);

    // 深睡等待电源键唤醒（浅睡 24h 超时后调用，不配置定时器）
    void enterDeepSleepOnButton();

    // 设置深睡定时（0=禁用，3-60s）
    void setDeepSleepTimer(uint32_t seconds);
    uint32_t deepSleepTimer() const { return deepSleepTimer_; }

    // 检查是否从深睡唤醒（用于启动时判断）
    static bool wokeFromDeepSleep();

    // 获取唤醒原因（深睡后调用）
    static esp_sleep_wakeup_cause_t wakeupCause();

private:
    SleepManager() = default;
    SleepManager(const SleepManager&) = delete;
    SleepManager& operator=(const SleepManager&) = delete;

    Pcf8563Rtc rtc_;
    bool rtcReady_ = false;
    uint32_t deepSleepTimer_ = 0;  // 0=禁用

    // 浅睡期间检测双击唤醒（700ms 窗口 + 50ms 去重）
    // 返回 true=检测到双击，false=超时或单击
    bool detectDoubleTap();

    // 配置 GPIO4 在深睡期间保持高电平
    void holdGpio4ForDeepSleep();
};

}  // namespace eego
