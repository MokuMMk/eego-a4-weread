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
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include <cstring>

namespace eego {

static const char* TAG = "eego.wifi";

#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1
#define WIFI_CONNECT_TIMEOUT_MS  10000   // 单次连接超时
#define WIFI_MAX_RETRIES    3            // 每个 SSID 最多重试次数

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

WifiState WifiManager::connect(const char* ssid, const char* password) {
    if (!initialized_) {
        if (init() != ESP_OK) {
            setState(WifiState::Failed);
            return state_;
        }
    }

    // 清除残留连接与事件位
    esp_wifi_disconnect();
    if (eventGroup_) {
        xEventGroupClearBits(eventGroup_, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);
    }
    lastReason_ = 0;

    wifi_config_t cfg = {};
    strncpy((char*)cfg.sta.ssid, ssid, sizeof(cfg.sta.ssid) - 1);
    if (password) {
        strncpy((char*)cfg.sta.password, password, sizeof(cfg.sta.password) - 1);
    }
    // 兼容开放网络与 WPA2/WPA3，使用最低阈值 OPEN，让驱动协商
    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;

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

    // 等待连接结果（最多 10s）
    EventBits_t bits = xEventGroupWaitBits(
        eventGroup_,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

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

    // 停止残留连接，避免扫描冲突
    esp_wifi_disconnect();

    Config::instance().load();
    const AppConfig& cfg = Config::instance().get();

    if (cfg.wifiCount == 0) {
        ESP_LOGW(TAG, "no wifi account in config, offline mode");
        setState(WifiState::OfflineMode);
        return state_;
    }

    // 扫描周边 AP（阻塞，约 1-2s）
    wifi_scan_config_t scanCfg = {};
    scanCfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scanCfg.show_hidden = false;
    ESP_LOGI(TAG, "scanning APs...");
    esp_err_t err = esp_wifi_scan_start(&scanCfg, true);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "scan failed: %s, offline mode", esp_err_to_name(err));
        setState(WifiState::OfflineMode);
        return state_;
    }

    uint16_t apNum = 0;
    if (esp_wifi_scan_get_ap_num(&apNum) != ESP_OK || apNum == 0) {
        ESP_LOGW(TAG, "no AP found, offline mode");
        setState(WifiState::OfflineMode);
        return state_;
    }

    wifi_ap_record_t* aps = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apNum);
    if (!aps) {
        setState(WifiState::OfflineMode);
        return state_;
    }
    if (esp_wifi_scan_get_ap_records(&apNum, aps) != ESP_OK) {
        free(aps);
        setState(WifiState::OfflineMode);
        return state_;
    }
    ESP_LOGI(TAG, "found %u APs", (unsigned)apNum);

    bool anySuccess = false;
    // 依次尝试 wifi_list 中可见的 SSID
    for (int i = 0; i < cfg.wifiCount && !anySuccess; i++) {
        const char* targetSsid = cfg.wifiList[i].ssid;
        if (targetSsid[0] == '\0') continue;

        // 检查是否在扫描结果中
        bool visible = false;
        for (int j = 0; j < apNum; j++) {
            if (strcmp((const char*)aps[j].ssid, targetSsid) == 0) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            ESP_LOGI(TAG, "SSID not visible: %s", targetSsid);
            continue;
        }

        // 最多重试 3 次
        for (int retry = 0; retry < WIFI_MAX_RETRIES && !anySuccess; retry++) {
            ESP_LOGI(TAG, "connecting %s (try %d/%d)",
                     targetSsid, retry + 1, WIFI_MAX_RETRIES);
            WifiState s = connect(targetSsid, cfg.wifiList[i].password);
            if (s == WifiState::Connected) {
                anySuccess = true;
                break;
            }
        }
    }

    free(aps);

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

void WifiManager::reconnectBackground() {
    if (!initialized_) {
        connectMulti();
        return;
    }
    if (currentSsid[0] == '\0') {
        connectMulti();
        return;
    }
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
