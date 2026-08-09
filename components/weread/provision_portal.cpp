// provision_portal.cpp — WiFi 配网门户实现（纯 ESP-IDF）
// AP "EEGO-A4-阅读器" + DNS 劫持（captive portal）+ HTTP 配网页 http://192.168.4.1
// 路由：GET /（页面）、GET /scan（扫描周边）、POST /connect（连接）、GET /status（状态）
// 任意其它路径 → 302 重定向到 /，触发手机 captive portal 弹窗。
// 连接成功后写入 config.json 的 wifi_list，NVS 由 esp_wifi 自动保存。
#include "provision_portal.h"
#include "wifi_manager.h"
#include "config.h"
#include "storage.h"

#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_http_server.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "lwip/netdb.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <errno.h>

namespace eego {

static const char* TAG = "eego.prov";

// ============================ 配网页面 HTML ============================
static const char PORTAL_HTML[] = R"HTML(<!DOCTYPE html>
<html lang="zh-CN">
<head>
<meta charset="UTF-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>EEGO A4 配网</title>
<style>
body{font-family:sans-serif;max-width:480px;margin:0 auto;padding:16px;background:#f5f5f5;color:#222}
h2{font-size:20px}
button{font-size:16px;padding:8px 20px;border:0;border-radius:6px;background:#222;color:#fff}
.ap{background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin:8px 0;cursor:pointer}
.ap:active{background:#e8e8e8}
.dim{color:#888}
#passbox{background:#fff;border:1px solid #ddd;border-radius:6px;padding:12px;margin-top:12px}
#pass{width:100%;box-sizing:border-box;font-size:16px;padding:8px;margin:8px 0;border:1px solid #bbb;border-radius:4px}
#st{margin-top:16px;font-size:15px;min-height:22px}
.ok{color:#1a7f37}
.err{color:#c00}
.spin{display:inline-block;width:14px;height:14px;border:2px solid #ccc;border-top-color:#333;border-radius:50%;animation:sp .8s linear infinite;vertical-align:middle}
@keyframes sp{to{transform:rotate(360deg)}}
</style>
</head>
<body>
<h2>EEGO A4 阅读器配网</h2>
<button onclick="scan()">刷新</button>
<div id="list" class="dim" style="margin-top:12px"></div>
<div id="passbox" style="display:none">
<div>网络：<b id="ssidv"></b></div>
<input id="pass" type="password" placeholder="WiFi 密码（开放网络留空）">
<button onclick="conn()">连接</button>
</div>
<div id="st"></div>
<script>
var sel=null,timer=null;
function scan(){
  var L=document.getElementById('list');
  L.textContent='扫描中…';
  fetch('/scan').then(function(r){return r.json();}).then(function(d){
    L.textContent='';
    if(!d.length){L.textContent='未发现网络，点刷新重试';return;}
    d.forEach(function(n){
      var div=document.createElement('div');
      div.className='ap';
      div.textContent=n.ssid+'  ('+n.rssi+' dBm)'+(n.secure?' 🔒':'');
      div.onclick=function(){pick(n);};
      L.appendChild(div);
    });
  }).catch(function(){L.textContent='扫描失败，点刷新重试';});
}
function pick(n){
  sel=n;
  document.getElementById('ssidv').textContent=n.ssid;
  document.getElementById('passbox').style.display='block';
  document.getElementById('pass').focus();
}
function conn(){
  if(!sel)return;
  var body='ssid='+encodeURIComponent(sel.ssid)+'&pass='+encodeURIComponent(document.getElementById('pass').value);
  fetch('/connect',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});
  document.getElementById('passbox').style.display='none';
  show('connecting');
  if(timer)clearInterval(timer);
  timer=setInterval(poll,2000);
}
function poll(){
  fetch('/status').then(function(r){return r.json();}).then(function(s){
    show(s.state,s);
    if(s.state==='ok'||s.state==='fail'){clearInterval(timer);timer=null;}
  }).catch(function(){});
}
function show(st,s){
  var el=document.getElementById('st');
  if(st==='connecting'){el.className='dim';el.innerHTML='<span class="spin"></span> 连接中，请稍候…';}
  else if(st==='ok'){el.className='ok';el.textContent='连接成功！IP: '+((s&&s.ip)||'')+'，配置已保存';}
  else if(st==='fail'){el.className='err';el.textContent='连接失败：'+((s&&s.reason)||'未知原因')+'（可重新选择网络重试）';}
  else{el.className='dim';el.textContent='';}
}
scan();
</script>
</body>
</html>)HTML";

// ============================ 失败原因码 → 中文 ============================
static const char* failReasonText(uint8_t reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND:
            return "找不到该网络（SSID 错误或信号差）";
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE:
            return "密码错误";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_NOT_AUTHED:
            return "认证失败";
        case WIFI_REASON_ASSOC_FAIL:
        case WIFI_REASON_ASSOC_EXPIRE:
            return "关联失败（AP 拒绝）";
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
            return "握手超时";
        case WIFI_REASON_BEACON_TIMEOUT:
        case WIFI_REASON_AUTH_LEAVE:
            return "连接被断开（信号差）";
        case 0:
            return "连接超时";
        default:
            return "连接失败";
    }
}

// ============================ URL 表单解析 ============================
// 从 "ssid=xxx&pass=yyy" 中提取字段并 URL decode
static bool getField(const char* body, const char* name,
                     char* out, size_t outSize) {
    out[0] = '\0';
    std::string s = body ? body : "";
    size_t pos = 0;
    while (pos <= s.size()) {
        size_t amp = s.find('&', pos);
        std::string kv = (amp == std::string::npos) ? s.substr(pos)
                                                    : s.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string key = kv.substr(0, eq);
            std::string val = kv.substr(eq + 1);
            if (key == name) {
                std::string dec;
                for (size_t i = 0; i < val.size(); i++) {
                    char c = val[i];
                    if (c == '+') {
                        c = ' ';
                    } else if (c == '%' && i + 2 < val.size()) {
                        char hex[3] = {val[i + 1], val[i + 2], 0};
                        c = (char)strtol(hex, nullptr, 16);
                        i += 2;
                    }
                    dec += c;
                }
                strncpy(out, dec.c_str(), outSize - 1);
                out[outSize - 1] = '\0';
                return true;
            }
        }
        if (amp == std::string::npos) break;
        pos = amp + 1;
    }
    return false;
}

// ============================ 单例 ============================
ProvisionPortal& ProvisionPortal::instance() {
    static ProvisionPortal inst;
    return inst;
}

// ============================ STA 连接事件（配网期间） ============================
void ProvisionPortal::wifiEventHandler(void* arg, esp_event_base_t base,
                                        int32_t id, void* data) {
    ProvisionPortal* self = &instance();
    (void)arg;

    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t* evt =
            (wifi_event_sta_disconnected_t*)data;
        if (self->provState_ == ST_CONNECTING) {
            strncpy(self->provReason_, failReasonText(evt->reason),
                    sizeof(self->provReason_) - 1);
            self->provReason_[sizeof(self->provReason_) - 1] = '\0';
            self->provState_ = ST_FAIL;
            ESP_LOGW(TAG, "portal: connect fail reason=%d", (int)evt->reason);
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* evt = (ip_event_got_ip_t*)data;
        if (self->provState_ == ST_CONNECTING) {
            snprintf(self->provIp_, sizeof(self->provIp_),
                     IPSTR, IP2STR(&evt->ip_info.ip));
            self->provState_ = ST_OK;
            ESP_LOGI(TAG, "portal: connect ok ip=%s", self->provIp_);
            self->saveWifiConfig();
        }
    }
}

// ============================ 写入 config.json ============================
void ProvisionPortal::saveWifiConfig() {
    Config::instance().load();
    AppConfig& cfg = Config::instance().mutableGet();

    // 重新配网时清空旧 WiFi 账号，只保留本次新配的账号，
    // 避免旧账号残留导致连接时优先尝试已失效的网络。
    cfg.wifiCount = 0;
    strncpy(cfg.wifiList[0].ssid, provSsid_,
            sizeof(cfg.wifiList[0].ssid) - 1);
    cfg.wifiList[0].ssid[sizeof(cfg.wifiList[0].ssid) - 1] = '\0';
    strncpy(cfg.wifiList[0].password, provPass_,
            sizeof(cfg.wifiList[0].password) - 1);
    cfg.wifiList[0].password[sizeof(cfg.wifiList[0].password) - 1] = '\0';
    cfg.wifiCount = 1;

    Storage::instance().saveConfig(cfg);
    ESP_LOGI(TAG, "portal: wifi config saved (%s)", provSsid_);
}

// ============================ 连接超时检查 ============================
void ProvisionPortal::checkTimeout() {
    if (provState_ != ST_CONNECTING) return;
    if (connectStart_ == 0) return;
    int64_t elapsed = esp_timer_get_time() - connectStart_;
    if (elapsed > 15000000LL) {  // 15s
        strncpy(provReason_, "连接超时", sizeof(provReason_) - 1);
        provReason_[sizeof(provReason_) - 1] = '\0';
        provState_ = ST_FAIL;
        ESP_LOGW(TAG, "portal: connect timeout");
        esp_wifi_disconnect();
    }
}

// ============================ 启动门户 ============================
esp_err_t ProvisionPortal::start() {
    if (running_) return ESP_OK;

    // 确保 wifi 驱动已初始化（WifiManager::init 已建 STA netif、注册事件、start）
    WifiManager::instance().init();

    // 已联网状态下重新配网：必须先断开 STA 连接，否则切 APSTA 模式时
    // STA 仍连着路由器，会导致 AP 启动异常 / beacon 不发 / 手机搜不到热点
    esp_wifi_disconnect();
    vTaskDelay(pdMS_TO_TICKS(200));  // 等底层完成断开

    // 创建 AP netif（仅一次）
    if (!apNetifCreated_) {
        esp_netif_create_default_wifi_ap();
        apNetifCreated_ = true;
    }

    // 切换到 APSTA 模式
    esp_err_t err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set APSTA mode failed: %s", esp_err_to_name(err));
        return err;
    }

    // 配置开放 AP
    wifi_config_t apCfg = {};
    const char* apSsid = "EEGO-A4-阅读器";
    strncpy((char*)apCfg.ap.ssid, apSsid, sizeof(apCfg.ap.ssid) - 1);
    apCfg.ap.ssid_len = (uint8_t)strlen(apSsid);
    apCfg.ap.channel = 1;
    apCfg.ap.authmode = WIFI_AUTH_OPEN;
    apCfg.ap.max_connection = 4;
    apCfg.ap.beacon_interval = 100;
    err = esp_wifi_set_config(WIFI_IF_AP, &apCfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set AP config failed: %s", esp_err_to_name(err));
        return err;
    }

    // 确保 wifi 已启动（模式切换后 AP 会生效）
    esp_wifi_start();

    // 注册配网 STA 事件处理器（与 WifiManager 并存，互不干扰）
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                               &wifiEventHandler, this);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifiEventHandler, this);
    handlerRegistered_ = true;

    provState_ = ST_IDLE;
    provReason_[0] = '\0';
    provIp_[0] = '\0';
    connectStart_ = 0;

    // 必须先置 running_=true，DNS 任务循环依赖它（否则任务立即退出）
    running_ = true;

    // DNS 劫持
    startDnsHijack();

    // HTTP 服务器
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 8;
    config.stack_size = 6144;
    config.uri_match_fn = httpd_uri_match_wildcard;  // 支持 "/*" 通配

    err = httpd_start(&server_, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start failed: %s", esp_err_to_name(err));
        stopDnsHijack();
        return err;
    }

    httpd_uri_t uriRoot = {};
    uriRoot.uri = "/";
    uriRoot.method = HTTP_GET;
    uriRoot.handler = handleRoot;
    httpd_register_uri_handler(server_, &uriRoot);

    httpd_uri_t uriScan = {};
    uriScan.uri = "/scan";
    uriScan.method = HTTP_GET;
    uriScan.handler = handleScan;
    httpd_register_uri_handler(server_, &uriScan);

    httpd_uri_t uriConn = {};
    uriConn.uri = "/connect";
    uriConn.method = HTTP_POST;
    uriConn.handler = handleConnect;
    httpd_register_uri_handler(server_, &uriConn);

    httpd_uri_t uriStat = {};
    uriStat.uri = "/status";
    uriStat.method = HTTP_GET;
    uriStat.handler = handleStatus;
    httpd_register_uri_handler(server_, &uriStat);

    // 任意路径 → 302 重定向（captive portal）
    httpd_uri_t uriAll = {};
    uriAll.uri = "/*";
    uriAll.method = HTTP_GET;
    uriAll.handler = handleNotFound;
    httpd_register_uri_handler(server_, &uriAll);

    ESP_LOGI(TAG, "provision portal started (AP: EEGO-A4-阅读器, http://192.168.4.1)");
    return ESP_OK;
}

// ============================ 停止门户 ============================
void ProvisionPortal::stop() {
    if (!running_) return;
    running_ = false;

    if (server_) {
        httpd_stop(server_);
        server_ = nullptr;
    }

    stopDnsHijack();

    if (handlerRegistered_) {
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     &wifiEventHandler);
        esp_event_handler_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     &wifiEventHandler);
        handlerRegistered_ = false;
    }

    // 切回 STA 模式
    esp_wifi_set_mode(WIFI_MODE_STA);

    ESP_LOGI(TAG, "provision portal stopped");
}

// ============================ DNS 劫持 ============================
void ProvisionPortal::startDnsHijack() {
    dnsSock_ = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (dnsSock_ < 0) {
        ESP_LOGE(TAG, "dns socket create failed");
        return;
    }

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(53);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(dnsSock_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "dns bind failed: errno=%d", (int)errno);
        close(dnsSock_);
        dnsSock_ = -1;
        return;
    }

    // 1s 接收超时，便于及时响应停止
    struct timeval tv = {};
    tv.tv_sec = 1;
    setsockopt(dnsSock_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    if (xTaskCreate(dnsTask, "dns_hijack", 4096, this, 5,
                    &dnsTaskHandle_) != pdPASS) {
        ESP_LOGE(TAG, "dns task create failed");
        close(dnsSock_);
        dnsSock_ = -1;
    }
}

void ProvisionPortal::stopDnsHijack() {
    // running_ 已在 stop() 中置 false，任务会在超时后退出
    if (dnsTaskHandle_) {
        vTaskDelay(pdMS_TO_TICKS(1200));
        dnsTaskHandle_ = nullptr;
    }
    if (dnsSock_ >= 0) {
        close(dnsSock_);
        dnsSock_ = -1;
    }
}

void ProvisionPortal::dnsTask(void* pvParameters) {
    ProvisionPortal* self = static_cast<ProvisionPortal*>(pvParameters);
    uint8_t rx[512];
    uint8_t tx[512];
    ESP_LOGI(TAG, "dns hijack task started (-> 192.168.4.1)");

    while (self->running_ && self->dnsSock_ >= 0) {
        struct sockaddr_in src;
        socklen_t srcLen = sizeof(src);
        int len = recvfrom(self->dnsSock_, rx, sizeof(rx), 0,
                           (struct sockaddr*)&src, &srcLen);
        if (len < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            break;
        }
        if (len < 17) continue;  // DNS 查询最小长度

        // 构造响应：复制查询，设 QR=1/AA=1/RA=1，ANSWER=1
        if (len > (int)sizeof(tx)) len = (int)sizeof(tx);
        memcpy(tx, rx, len);
        tx[2] = 0x81;  // QR=1, Opcode=0, AA=1, TC=0, RD=1
        tx[3] = 0x80;  // RA=1, Z=0, RCODE=0 (No error)
        tx[6] = 0; tx[7] = 1;     // ANCOUNT = 1
        tx[8] = 0; tx[9] = 0;     // NSCOUNT = 0
        tx[10] = 0; tx[11] = 0;   // ARCOUNT = 0

        // 定位 question 段结束（name + null + type(2) + class(2)）
        int qEnd = 12;
        while (qEnd < len && rx[qEnd] != 0) {
            qEnd += (rx[qEnd] + 1);
        }
        qEnd += 5;  // null + type + class
        if (qEnd > len) continue;

        int pos = qEnd;
        if (pos + 16 > (int)sizeof(tx)) continue;

        // Answer: 指针指向问题中的域名（偏移 12 → 0xC00C）
        tx[pos++] = 0xC0; tx[pos++] = 0x0C;
        tx[pos++] = 0; tx[pos++] = 1;          // TYPE = A
        tx[pos++] = 0; tx[pos++] = 1;          // CLASS = IN
        tx[pos++] = 0; tx[pos++] = 0; tx[pos++] = 0; tx[pos++] = 60;  // TTL = 60
        tx[pos++] = 0; tx[pos++] = 4;          // RDLENGTH = 4
        tx[pos++] = 192; tx[pos++] = 168; tx[pos++] = 4; tx[pos++] = 1;  // 192.168.4.1

        sendto(self->dnsSock_, tx, pos, 0,
               (struct sockaddr*)&src, srcLen);
    }

    ESP_LOGI(TAG, "dns hijack task exiting");
    vTaskDelete(nullptr);
}

// ============================ HTTP 路由 ============================
esp_err_t ProvisionPortal::handleRoot(httpd_req_t* req) {
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_send(req, PORTAL_HTML, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t ProvisionPortal::handleScan(httpd_req_t* req) {
    // 阻塞扫描（APSTA 模式下扫描 STA 侧，不影响 AP）
    wifi_scan_config_t scanCfg = {};
    scanCfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    esp_err_t err = esp_wifi_scan_start(&scanCfg, true);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    uint16_t apNum = 0;
    esp_wifi_scan_get_ap_num(&apNum);
    wifi_ap_record_t* aps = nullptr;
    if (apNum > 0) {
        aps = (wifi_ap_record_t*)malloc(sizeof(wifi_ap_record_t) * apNum);
        if (aps) {
            esp_wifi_scan_get_ap_records(&apNum, aps);
        }
    }

    cJSON* arr = cJSON_CreateArray();
    // 同名 SSID 合并：只保留信号最强的一条，避免列表出现重复网络
    for (int i = 0; i < apNum && aps; i++) {
        if (aps[i].ssid[0] == '\0') continue;  // 跳过隐藏 SSID
        const char* ssidName = (const char*)aps[i].ssid;
        // 查找是否已有同名条目
        bool merged = false;
        int n = cJSON_GetArraySize(arr);
        for (int j = 0; j < n; j++) {
            cJSON* existing = cJSON_GetArrayItem(arr, j);
            cJSON* eSsid = cJSON_GetObjectItem(existing, "ssid");
            if (eSsid && strcmp(eSsid->valuestring, ssidName) == 0) {
                // 已有同名：若当前信号更强则更新 rssi
                cJSON* eRssi = cJSON_GetObjectItem(existing, "rssi");
                if (eRssi && aps[i].rssi > eRssi->valuedouble) {
                    cJSON_SetNumberValue(eRssi, aps[i].rssi);
                }
                merged = true;
                break;
            }
        }
        if (merged) continue;
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", ssidName);
        cJSON_AddNumberToObject(o, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(o, "secure", aps[i].authmode != WIFI_AUTH_OPEN);
        cJSON_AddItemToArray(arr, o);
    }
    free(aps);

    char* str = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);

    httpd_resp_set_type(req, "application/json");
    if (str) {
        httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
        free(str);
    } else {
        httpd_resp_send(req, "[]", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

esp_err_t ProvisionPortal::handleConnect(httpd_req_t* req) {
    char buf[384] = {};
    int len = httpd_req_recv(req, buf, sizeof(buf) - 1);
    if (len <= 0) {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"state\":\"fail\",\"reason\":\"未收到数据\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }
    buf[len] = '\0';

    char ssid[33] = {};
    char pass[65] = {};
    getField(buf, "ssid", ssid, sizeof(ssid));
    getField(buf, "pass", pass, sizeof(pass));

    if (ssid[0] == '\0') {
        httpd_resp_set_type(req, "application/json");
        httpd_resp_send(req,
            "{\"state\":\"fail\",\"reason\":\"缺少 ssid\"}",
            HTTPD_RESP_USE_STRLEN);
        return ESP_OK;
    }

    ProvisionPortal& p = instance();
    strncpy(p.provSsid_, ssid, sizeof(p.provSsid_) - 1);
    p.provSsid_[sizeof(p.provSsid_) - 1] = '\0';
    strncpy(p.provPass_, pass, sizeof(p.provPass_) - 1);
    p.provPass_[sizeof(p.provPass_) - 1] = '\0';
    p.provReason_[0] = '\0';
    p.provIp_[0] = '\0';
    p.provState_ = ST_CONNECTING;
    p.connectStart_ = esp_timer_get_time();

    // 配置 STA 并发起连接（APSTA 模式下 STA 侧）
    wifi_config_t staCfg = {};
    strncpy((char*)staCfg.sta.ssid, ssid, sizeof(staCfg.sta.ssid) - 1);
    strncpy((char*)staCfg.sta.password, pass, sizeof(staCfg.sta.password) - 1);
    staCfg.sta.threshold.authmode = WIFI_AUTH_WPA_PSK;  // 支持 WPA/WPA2/WPA3 及 TKIP/AES
    esp_wifi_set_config(WIFI_IF_STA, &staCfg);
    esp_wifi_disconnect();
    esp_wifi_connect();

    ESP_LOGI(TAG, "portal: connecting to %s", ssid);

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, "{\"state\":\"connecting\"}", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

esp_err_t ProvisionPortal::handleStatus(httpd_req_t* req) {
    ProvisionPortal& p = instance();
    p.checkTimeout();  // 顺带处理超时

    const char* st;
    switch (p.provState_) {
        case ST_CONNECTING: st = "connecting"; break;
        case ST_OK:         st = "ok"; break;
        case ST_FAIL:       st = "fail"; break;
        default:            st = "idle"; break;
    }

    cJSON* o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "state", st);
    cJSON_AddStringToObject(o, "reason", p.provReason_);
    cJSON_AddStringToObject(o, "ssid", p.provSsid_);
    cJSON_AddStringToObject(o, "ip", p.provIp_);

    char* str = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);

    httpd_resp_set_type(req, "application/json");
    if (str) {
        httpd_resp_send(req, str, HTTPD_RESP_USE_STRLEN);
        free(str);
    } else {
        httpd_resp_send(req, "{\"state\":\"idle\"}", HTTPD_RESP_USE_STRLEN);
    }
    return ESP_OK;
}

esp_err_t ProvisionPortal::handleNotFound(httpd_req_t* req) {
    // 任意路径 → 302 重定向到门户首页，触发手机 captive 弹窗
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_send(req, "", 0);
    return ESP_OK;
}

}  // namespace eego
