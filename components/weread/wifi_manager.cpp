// wifi_manager.cpp — WiFi 管理器实现（纯 ESP-IDF）
// STA 模式连接、扫描、多账号匹配、离线模式、省电。
// 事件：WIFI_EVENT_STA_DISCONNECTED / IP_EVENT_STA_GOT_IP
// 连接成功后凭据由 esp_wifi 自动写入 NVS（CONFIG_ESP32_WIFI_NVS_ENABLED=y）。
#include "wifi_manager.h"
#include "config.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstring>

namespace eego {

static const char* TAG = "eego.wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_CONNECT_TIMEOUT_MS  15000   // 单次连接超时（含关联+握手+DHCP，弱信号下放宽）
#define WIFI_MAX_RETRIES    3            // 每个 SSID 最多重试次数
#define WIFI_RECONNECT_INTERVAL_MS 30000 // 离线/掉线后台自动重连的最小间隔

WifiManager& WifiManager::instance() {
    static WifiManager inst;
    return inst;
}

esp_err_t WifiManager::init() {
    if (initialized_) return ESP_OK;

    // TCP/IP 协议栈（已初始化则跳过）
    esp_err_t err = esp_netif_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 默认事件循环（已存在则跳过）
    err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "event_loop_create failed: %s", esp_err_to_name(err));
        return err;
    }

    // 创建默认 STA netif
    esp_netif_create_default_wifi_sta();

    // WiFi 驱动初始化
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 注册事件处理器
    err = esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                     &wifiEventHandler, this);
    if (err != ESP_OK) return err;
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifiEventHandler, this);
    if (err != ESP_OK) return err;

    // STA 模式并启动
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) return err;
    err = esp_wifi_start();
    if (err != ESP_OK) return err;

    // 关闭省电模式：弱信号下默认的 WIFI_PS_MIN_MODEM 会导致 beacon 监听间隔拉长，
    // 频繁出现 4-way handshake 超时(reason=15)/auth 超时(reason=4)/关联断开(reason=8)。
    // WIFI_PS_NONE 让 STA 始终保持接收，代价是功耗增加约 100-200mA。
    esp_wifi_set_ps(WIFI_PS_NONE);
    // TX 功率由 sdkconfig 的 CONFIG_ESP_PHY_MAX_WIFI_TX_POWER=20 (dBm) 控制，
    // 已是 ESP32-S3 硬件/法规上限，无需运行时再设（esp_wifi_set_max_tx_power
    // 参数单位是 0.25dBm，误传 20 会变成 5dBm 反而大幅降低功率）。

    eventGroup_ = xEventGroupCreate();
    if (!eventGroup_) return ESP_ERR_NO_MEM;

    initialized_ = true;
    ESP_LOGI(TAG, "wifi initialized (STA mode)");
    return ESP_OK;
}

void WifiManager::setState(WifiState s) {
    if (state_ == s) return;
    state_ = s;
    ESP_LOGI(TAG, "state -> %d", (int)s);
    if (callback_) callback_(s);
}

void WifiManager::wifiEventHandler(void* arg, esp_event_base_t base,
                                    int32_t id, void* data) {
    WifiManager* self = static_cast<WifiManager*>(arg);

    if (base == WIFI_EVENT) {
        switch (id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started");
                break;
            case WIFI_EVENT_STA_CONNECTED: {
                wifi_event_sta_connected_t* evt = (wifi_event_sta_connected_t*)data;
                strncpy(self->currentSsid, (const char*)evt->ssid,
                        sizeof(self->currentSsid) - 1);
                self->currentSsid[sizeof(self->currentSsid) - 1] = '\0';
                break;
            }
            case WIFI_EVENT_STA_DISCONNECTED: {
                wifi_event_sta_disconnected_t* evt =
                    (wifi_event_sta_disconnected_t*)data;
                ESP_LOGW(TAG, "STA disconnected, reason=%d", (int)evt->reason);
                if (self->connecting_) {
                    // connect() 正在等待：报告失败
                    self->lastReason_ = evt->reason;
                    xEventGroupSetBits(self->eventGroup_, WIFI_FAIL_BIT);
                } else {
                    // 非主动连接期间掉线
                    self->setState(WifiState::Disconnected);
                }
                break;
            }
            default:
                break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* evt = (ip_event_got_ip_t*)data;
        ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&evt->ip_info.ip));
        if (self->connecting_) {
            xEventGroupSetBits(self->eventGroup_, WIFI_CONNECTED_BIT);
        }
        self->setState(WifiState::Connected);
    }
}

WifiState WifiManager::connect(const char* ssid, const char* password, uint32_t timeout_ms,
                               const uint8_t* bssid, int channel) {
    if (!initialized_) {
        if (init() != ESP_OK) {
            setState(WifiState::Failed);
            return state_;
        }
    }

    // 清除残留连接与事件位
    esp_wifi_disconnect();
    // 等底层完成断开，避免紧随其后的 esp_wifi_connect() 触发驱动竞态
    vTaskDelay(pdMS_TO_TICKS(200));
    if (eventGroup_) {
        xEventGroupClearBits(eventGroup_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    lastReason_ = 0;

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    // 认证阈值设为 WPA_PSK：接受 WPA / WPA2 / WPA3 及 TKIP / AES 等常用加密，
    // 而非仅限 WPA2，扩大对不同路由器协议的兼容。
    // 注意：阈值不是上层开关，不能设为 OPEN（会限制 STA 只能连开放网络）。
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;

    // 同名多 AP/mesh 场景：固定到扫描到的最强 BSSID 与信道，
    // 避免 esp_wifi_connect() 因信道/AP 歧义导致认证超时(reason=2)。
    if (bssid && channel > 0 && channel < 15) {
        memcpy(cfg.sta.bssid, bssid, 6);
        cfg.sta.bssid_set = true;
        cfg.sta.channel = (uint8_t)channel;
    }

    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set_config failed: %s", esp_err_to_name(err));
        setState(WifiState::Failed);
        return state_;
    }

    strncpy(currentSsid, ssid, sizeof(currentSsid) - 1);
    currentSsid[sizeof(currentSsid) - 1] = '\0';

    connecting_ = true;
    setState(WifiState::Connecting);

    err = esp_wifi_connect();
    if (err != ESP_OK) {
        connecting_ = false;
        ESP_LOGE(TAG, "esp_wifi_connect failed: %s", esp_err_to_name(err));
        setState(WifiState::Failed);
        return state_;
    }

    // 等待连接结果
    EventBits_t bits = xEventGroupWaitBits(
        eventGroup_,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_ms));

    connecting_ = false;

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "connected to %s", ssid);
        return state_;  // Connected
    }

    // 失败或超时
    if (!(bits & WIFI_FAIL_BIT)) {
        // 超时：主动断开停止底层尝试
        esp_wifi_disconnect();
        lastReason_ = 0;
        ESP_LOGW(TAG, "connect timeout to %s", ssid);
    } else {
        ESP_LOGW(TAG, "connect failed to %s, reason=%d", ssid, (int)lastReason_);
    }
    setState(WifiState::Failed);
    return state_;
}

WifiState WifiManager::connectMulti() {
    if (!initialized_) {
        if (init() != ESP_OK) {
            setState(WifiState::OfflineMode);
            return state_;
        }
    }

    Config::instance().load();
    const AppConfig& cfg = Config::instance().get();

    if (cfg.wifiCount == 0) {
        ESP_LOGW(TAG, "no wifi account in config, offline mode");
        setState(WifiState::OfflineMode);
        return state_;
    }

    // 停止残留连接，避免扫描冲突
    // （在 wifiCount 检查之后执行：配网中 wifiCount==0 时不触碰 WiFi 状态，
    //   避免后台自动重连任务在配网期间误断开正在进行的 STA 连接）
    esp_wifi_disconnect();

    // 扫描周边 AP（阻塞，约 1-2s）。仅用于提示/排序，不作为连接前置条件：
    // 弱信号/干扰/隐藏网络常漏检，直接按名连接所有已配置 SSID 更可靠。
    wifi_scan_config_t scanCfg = {};
    scanCfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scanCfg.show_hidden = true;  // 支持隐藏 SSID
    ESP_LOGI(TAG, "scanning APs...");
    esp_err_t err = esp_wifi_scan_start(&scanCfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s, 继续按名直连", esp_err_to_name(err));
    }

    uint16_t apNum = 0;
    wifi_ap_record_t* aps = nullptr;
    if (esp_wifi_scan_get_ap_num(&apNum) != ESP_OK) apNum = 0;
    if (apNum > 0) {
        aps = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apNum);
        if (aps && esp_wifi_scan_get_ap_records(&apNum, aps) != ESP_OK) {
            free(aps); aps = nullptr; apNum = 0;
        }
        if (aps) {
            ESP_LOGI(TAG, "found %u APs:", (unsigned)apNum);
            for (int j = 0; j < apNum; j++) {
                if (aps[j].ssid[0] == '\0') continue;
                ESP_LOGI(TAG, "  AP[%d] %s rssi=%d ch=%d auth=%d", j,
                         (const char*)aps[j].ssid, aps[j].rssi, aps[j].primary,
                         (unsigned)aps[j].authmode);
            }
        }
    }

    bool anySuccess = false;
    // 依次直接尝试所有已配置 SSID（按名寻网，不依赖扫描结果）
    for (int i = 0; i < cfg.wifiCount && !anySuccess; i++) {
        const char* targetSsid = cfg.wifiList[i].ssid;
        if (targetSsid[0] == '\0') continue;

        // 提示：该 SSID 是否在扫描结果中可见（仅诊断，不影响连接）
        bool visible = false;
        if (aps) {
            for (int j = 0; j < apNum; j++) {
                if (strcmp((const char*)aps[j].ssid, targetSsid) == 0) {
                    visible = true;
                    break;
                }
            }
        }
        ESP_LOGI(TAG, "尝试连接 %s (扫描可见=%d)", targetSsid, (int)visible);

        // 按名直连，不固定最强 BSSID/信道。
        // 与配网页(provision_portal)的连接方式保持一致，该路径已验证可用；
        // 访客网络/虚拟多 BSSID 场景下固定到单个 BSSID 反而会触发认证失败
        // (AUTH_FAIL reason=4 / AUTH_EXPIRE reason=2)。
        // 最多重试 3 次，第一次给 30s 容忍弱信号慢握手，后续 15s
        for (int retry = 0; retry < WIFI_MAX_RETRIES && !anySuccess; retry++) {
            uint32_t tmo = (retry == 0) ? 30000 : 15000;
            ESP_LOGI(TAG, "connecting %s (try %d/%d, timeout=%lums)",
                     targetSsid, retry + 1, WIFI_MAX_RETRIES, (unsigned long)tmo);
            WifiState s = connect(targetSsid, cfg.wifiList[i].password, tmo);
            if (s == WifiState::Connected) {
                ESP_LOGI(TAG, "连接成功 %s, reason=%d", targetSsid, (int)lastReason_);
                anySuccess = true;
                break;
            }
            ESP_LOGW(TAG, "连接失败 %s (try %d/%d), reason=%d",
                     targetSsid, retry + 1, WIFI_MAX_RETRIES, (int)lastReason_);
        }
    }

    if (aps) free(aps);

    if (anySuccess) {
        return state_;  // Connected
    }

    ESP_LOGW(TAG, "all wifi accounts failed, offline mode");
    setState(WifiState::OfflineMode);
    return state_;
}

void WifiManager::disconnect() {
    connecting_ = false;
    if (initialized_) {
        esp_wifi_disconnect();
    }
    setState(WifiState::Disconnected);
}

void WifiManager::powerOff() {
    connecting_ = false;
    if (initialized_) {
        esp_wifi_disconnect();
        esp_wifi_stop();
    }
    setState(WifiState::Disconnected);
    ESP_LOGI(TAG, "wifi powered off");
}

void WifiManager::markReconnectAttempt() {
    lastReconnectAt_ = esp_timer_get_time() / 1000;  // 毫秒
}

bool WifiManager::backgroundReconnectDue() const {
    if (state_ == WifiState::Connected) return false;
    int64_t nowMs = esp_timer_get_time() / 1000;
    return (nowMs - lastReconnectAt_) >= WIFI_RECONNECT_INTERVAL_MS;
}

void WifiManager::reconnectBackground() {
    if (!initialized_) {
        connectMulti();
        return;
    }
    if (currentSsid[0] == '\0') {
        connectMulti();
        return;
    }
    // 确保驱动已启动：休眠时 powerOff() 会 esp_wifi_stop()，
    // 唤醒后若不先 start 就 connect 会报 ESP_ERR_WIFI_NOT_STARTED。
    // esp_wifi_start() 在已启动时返回 ESP_ERR_INVALID_STATE，属正常，忽略。
    esp_err_t startErr = esp_wifi_start();
    if (startErr != ESP_OK && startErr != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "reconnect: esp_wifi_start failed: %s", esp_err_to_name(startErr));
        setState(WifiState::Disconnected);
        return;
    }
    // esp_wifi_start() 会把省电模式重置为默认的 WIFI_PS_MIN_MODEM，
    // 需重新强制 WIFI_PS_NONE，否则弱信号下 beacon 监听间隔拉长导致连接超时
    // （日志中表现为 wifipm 进入 modem sleep）。
    esp_wifi_set_ps(WIFI_PS_NONE);
    // 后台重连：不阻塞，不设 connecting_（事件走掉线分支更新状态）
    if (eventGroup_) {
        xEventGroupClearBits(eventGroup_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    setState(WifiState::Connecting);
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "background reconnect failed: %s", esp_err_to_name(err));
        setState(WifiState::Disconnected);
        return;
    }
    ESP_LOGI(TAG, "background reconnect to %s", currentSsid);
}

int8_t WifiManager::rssi() const {
    if (!initialized_ || state_ != WifiState::Connected) return 0;
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        return ap.rssi;
    }
    return 0;
}

}  // namespace eego
