// provision_portal.h — WiFi 配网门户（纯 ESP-IDF）
// AP "EEGO-A4-阅读器"（开放）+ DNS 劫持（captive portal）+ HTTP 配网页 http://192.168.4.1
// 成功后把 SSID/密码写入 config.json 的 wifi_list，并保存到 NVS。
#pragma once
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace eego {

class ProvisionPortal {
public:
    static ProvisionPortal& instance();

    // 启动配网门户：
    // 1. 切换为 APSTA 模式（AP SSID: EEGO-A4-阅读器，开放）
    // 2. 启动 DNS 劫持（所有域名解析到 192.168.4.1）
    // 3. 启动 HTTP 服务器：GET /scan、POST /connect、GET /status、GET /
    // 4. captive portal 自动弹窗（任意路径 302 → /）
    esp_err_t start();

    // 停止配网门户（关 HTTP/DNS，切回 STA 模式）
    void stop();

    bool isRunning() const { return running_; }

private:
    ProvisionPortal() = default;

    enum ProvState { ST_IDLE, ST_CONNECTING, ST_OK, ST_FAIL };

    static esp_err_t handleRoot(httpd_req_t* req);
    static esp_err_t handleScan(httpd_req_t* req);
    static esp_err_t handleConnect(httpd_req_t* req);
    static esp_err_t handleStatus(httpd_req_t* req);
    static esp_err_t handleNotFound(httpd_req_t* req);

    static void wifiEventHandler(void* arg, esp_event_base_t base,
                                  int32_t id, void* data);
    static void dnsTask(void* pvParameters);

    void startDnsHijack();
    void stopDnsHijack();
    void saveWifiConfig();      // 把当前 provSsid_/provPass_ 写入 config.json
    void checkTimeout();        // 连接超时检查（15s）

    bool running_ = false;
    httpd_handle_t server_ = nullptr;

    volatile ProvState provState_ = ST_IDLE;
    char provSsid_[33] = {};
    char provPass_[65] = {};
    char provReason_[64] = {};   // 中文失败原因
    char provIp_[16] = {};
    int64_t connectStart_ = 0;   // esp_timer_get_time() 微秒

    TaskHandle_t dnsTaskHandle_ = nullptr;
    int dnsSock_ = -1;
    bool apNetifCreated_ = false;
    bool handlerRegistered_ = false;
};

}  // namespace eego
