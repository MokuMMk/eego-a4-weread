// wifi_manager.h — WiFi 管理器（纯 ESP-IDF，STA 模式）
// 负责连接/重连/扫描，多账号自动匹配，离线模式，省电关闭。
// 配网由 ProvisionPortal 负责；本类只管 STA 侧连接。
#pragma once
#include "esp_err.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstdint>

namespace eego {

enum class WifiState {
    Disconnected,   // 未连接
    Connecting,     // 正在连接
    Connected,      // 已连接并拿到 IP
    Failed,         // 连接失败（单次）
    OfflineMode     // 离线模式（全部账号失败后进入，不自动重连）
};

class WifiManager {
public:
    static WifiManager& instance();

    // 初始化 esp_wifi（STA 模式），注册事件，启动 wifi。可重复调用。
    esp_err_t init();

    // 从 config.json 读取 wifi_list，扫描可见 AP，连第一个匹配的。
    // 每个匹配 SSID 最多重试 3 次（第一次 30s、后续 15s），全失败后进入离线模式。
    WifiState connectMulti();

    // 连接指定 SSID/密码（阻塞）。timeout_ms 为单次连接超时（含关联+握手+DHCP）。
    // bssid(channel) 用于同名多 AP/mesh 时固定到扫描到的最强实例，避免认证超时(reason=2)。
    WifiState connect(const char* ssid, const char* password,
                      uint32_t timeout_ms = 15000,
                      const uint8_t* bssid = nullptr, int channel = 0);

    // 断开连接
    void disconnect();

    // 断开 WiFi 并停止驱动（省电模式，用于休眠）
    void powerOff();

    // 后台静默重连（唤醒后调用，不弹 UI，不阻塞）
    void reconnectBackground();

    // 后台自动重连节流：距上次尝试是否已超过 30s（离线/掉线时事件循环据此触发重连）
    bool backgroundReconnectDue() const;
    // 记录一次后台重连尝试时间（事件循环在触发重连后调用）
    void markReconnectAttempt();

    // 状态查询
    WifiState state() const { return state_; }
    bool isConnected() const { return state_ == WifiState::Connected; }
    int8_t rssi() const;

    // 当前连接的 SSID（连接成功后填充）
    char currentSsid[33] = {};

    // 状态变化回调
    using StateCallback = void (*)(WifiState newState);
    void onStateChange(StateCallback cb) { callback_ = cb; }

private:
    WifiManager() = default;
    WifiState state_ = WifiState::Disconnected;
    StateCallback callback_ = nullptr;
    bool initialized_ = false;
    int64_t lastReconnectAt_ = 0;  // 上次后台自动重连尝试时间（毫秒）

    // 同步 connect() 等待事件用
    EventGroupHandle_t eventGroup_ = nullptr;
    volatile bool connecting_ = false;
    uint8_t lastReason_ = 0;

    void setState(WifiState s);
    static void wifiEventHandler(void* arg, esp_event_base_t base,
                                  int32_t id, void* data);
};

}  // namespace eego
