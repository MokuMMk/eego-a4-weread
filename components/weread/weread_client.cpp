// weread_client.cpp — 实现
// 纯 ESP-IDF：esp_http_client + esp_crt_bundle + cJSON + SD 卡 config.json。
// WiFi 连接由 WifiManager 负责，本类只管 HTTP + cookie。
#include "weread_client.h"
#include "config.h"
#include "storage.h"
#include "sd_card.h"

#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "cJSON.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>

namespace eego {

static const char* TAG = "eego.wr";

WereadClient WR;

const char* WEREAD_USER_AGENT =
    "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
    "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/135.0.0.0 "
    "Safari/537.36 Edg/135.0.0.0";

// 毫秒时间戳（替代 Arduino millis()）
static inline int64_t millis_now() { return esp_timer_get_time() / 1000; }

// 在 [data,len) 内查找子串 needle，返回下标或 -1（不要求 \0 结尾）
static int find_sub(const char* data, size_t len, const char* needle, size_t nlen) {
    if (!nlen || nlen > len) return -1;
    for (size_t i = 0; i + nlen <= len; i++)
        if (memcmp(data + i, needle, nlen) == 0) return (int)i;
    return -1;
}

// ---- HttpResponse 所有权实现 ----
HttpResponse::~HttpResponse() { if (psbody) heap_caps_free(psbody); }
HttpResponse::HttpResponse(HttpResponse&& o) noexcept
    : status(o.status), body(std::move(o.body)), headers(std::move(o.headers)),
      error(std::move(o.error)), psbody(o.psbody), pslen(o.pslen) {
    o.psbody = nullptr; o.pslen = 0;
}
HttpResponse& HttpResponse::operator=(HttpResponse&& o) noexcept {
    if (this != &o) {
        if (psbody) heap_caps_free(psbody);
        status = o.status; body = std::move(o.body); headers = std::move(o.headers);
        error = std::move(o.error); psbody = o.psbody; pslen = o.pslen;
        o.psbody = nullptr; o.pslen = 0;
    }
    return *this;
}

WereadClient::~WereadClient() { sessClose_(); }

// esp_http_client 事件回调里累积响应体
// 大响应写入 PSRAM buffer（std::string 超大响应也走 PSRAM 缓冲）
struct RespBuf {
    std::string* body;
    std::map<std::string,std::string>* headers;
    char* psbuf = nullptr;   // PSRAM 接收缓冲
    size_t pslen = 0;        // 已写入长度
    size_t pscap = 0;        // 缓冲容量
    bool overflow = false;   // 扩容到顶仍放不下 → 数据被截断，不能当成功响应用
};

// 接收缓冲：初始 512KB，不够按需翻倍，上限 4MB（PSRAM 共 8MB）
#define HTTP_BUF_INIT (512 * 1024)
#define HTTP_BUF_MAX  (4 * 1024 * 1024)

static esp_err_t http_event_handler(esp_http_client_event_t* evt) {
    RespBuf* rb = (RespBuf*)evt->user_data;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA:
            if (rb && evt->data_len > 0) {
                // 优先写 PSRAM buffer
                if (rb->psbuf) {
                    size_t need = rb->pslen + evt->data_len;
                    if (need > rb->pscap) { // 放不下 → 翻倍扩容（封顶 HTTP_BUF_MAX）
                        size_t newcap = rb->pscap;
                        while (newcap < need && newcap < HTTP_BUF_MAX) newcap *= 2;
                        if (newcap > HTTP_BUF_MAX) newcap = HTTP_BUF_MAX;
                        if (newcap >= need) {
                            char* nb = (char*)heap_caps_realloc(rb->psbuf, newcap, MALLOC_CAP_SPIRAM);
                            if (nb) {
                                ESP_LOGI(TAG, "接收缓冲扩容 %u→%u KB",
                                         (unsigned)(rb->pscap / 1024), (unsigned)(newcap / 1024));
                                rb->psbuf = nb; rb->pscap = newcap;
                            } else {
                                ESP_LOGE(TAG, "PSRAM 扩容到 %u KB 失败", (unsigned)(newcap / 1024));
                            }
                        }
                    }
                    if (rb->pslen + evt->data_len <= rb->pscap) {
                        memcpy(rb->psbuf + rb->pslen, evt->data, evt->data_len);
                        rb->pslen += evt->data_len;
                    } else {
                        rb->overflow = true; // 到顶仍放不下：宁可报错，绝不静默截断
                    }
                } else if (rb->body) {
                    rb->body->append((const char*)evt->data, evt->data_len);
                }
            }
            break;
        case HTTP_EVENT_ON_HEADER:
            if (rb && rb->headers && evt->header_key && evt->header_value) {
                std::string k = evt->header_key;
                std::transform(k.begin(), k.end(), k.begin(),
                               [](unsigned char c){ return std::tolower(c); });
                std::string v = evt->header_value;
                // set-cookie 可能多条，用换行拼接
                auto it = rb->headers->find("set-cookie");
                if (k == "set-cookie" && it != rb->headers->end())
                    it->second += "\n" + v;
                else
                    (*rb->headers)[k] = v;
            }
            break;
        default:
            break;
    }
    return ESP_OK;
}

// ---- config.json cookie 读写（cJSON + SdCard，保留未知字段）----

// 读 config.json 整文件到 std::string；失败返回空
static bool read_config_json(std::string& out) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !sd.exists(SD_CONFIG_PATH)) return false;
    FILE* f = sd.open(SD_CONFIG_PATH, "rb");
    if (!f) return false;
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        out.append(chunk, n);
    }
    fclose(f);
    return !out.empty();
}

// 把 std::string 序列化的 JSON 写回 config.json
static bool write_config_json(const char* str) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;
    sd.mkdir(SD_WEREAD_DIR);
    return sd.writeFile(SD_CONFIG_PATH, (const uint8_t*)str, strlen(str));
}

// cJSON 字段取字符串（NULL 安全）
static const char* cj_str(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_GetStringValue(item);
}

bool WereadClient::loadConfig() {
    std::string buf;
    if (!read_config_json(buf)) {
        ESP_LOGW(TAG, "config.json 不存在或读取失败，cookie 为空");
        return false;
    }
    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) {
        ESP_LOGW(TAG, "config.json JSON 解析失败");
        return false;
    }

    const char* vid  = cj_str(root, "wr_vid");
    const char* skey = cj_str(root, "wr_skey");
    const char* rt   = cj_str(root, "wr_rt");
    const char* key  = cj_str(root, "api_key");

    if (vid  && vid[0])  cookies_["wr_vid"]  = vid;
    if (skey && skey[0]) cookies_["wr_skey"] = skey;
    if (rt   && rt[0])   cookies_["wr_rt"]   = rt;
    if (key  && key[0])  cfg_api_key = key;

    cJSON_Delete(root);
    ESP_LOGI(TAG, "cookie 加载: vid=%s skey=%s rt=%s",
             vid ? "有" : "无", skey ? "有" : "无", rt ? "有" : "无");
    return true;
}

void WereadClient::setCookie(const std::string& name, const std::string& value) {
    if (!value.empty()) cookies_[name] = value;
    if (name == "wr_rt") rt_dead_ = false; // 新 rt 到手（扫码登录/服务器轮换）即复活
}

std::string WereadClient::getCookie(const std::string& name) const {
    auto it = cookies_.find(name);
    return it == cookies_.end() ? std::string() : it->second;
}

bool WereadClient::saveCookiesToConfig() {
    // 读出原有配置（保留 wifi_ssid/wifi_pass/api_key 等），只更新 wr_* 三个字段
    std::string buf;
    cJSON* root = nullptr;
    if (read_config_json(buf)) {
        root = cJSON_Parse(buf.c_str());
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) {
        ESP_LOGE(TAG, "保存 cookie 失败：无法创建 JSON");
        return false;
    }

    // 先删旧 key 再添加，避免重复
    auto set_str = [&](const char* key, const std::string& val) {
        cJSON_DeleteItemFromObject(root, key);
        cJSON_AddStringToObject(root, key, val.c_str());
    };
    set_str("wr_vid",  getCookie("wr_vid"));
    set_str("wr_skey", getCookie("wr_skey"));
    set_str("wr_rt",   getCookie("wr_rt"));

    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) {
        ESP_LOGE(TAG, "cJSON_Print 失败");
        return false;
    }
    bool ok = write_config_json(str);
    free(str);
    if (ok) ESP_LOGI(TAG, "cookie 已保存到 config.json");
    else    ESP_LOGE(TAG, "写 config.json 失败");
    return ok;
}

std::string WereadClient::cookieHeader() const {
    std::string h;
    for (auto& kv : cookies_) {
        if (!h.empty()) h += "; ";
        h += kv.first + "=" + kv.second;
    }
    return h;
}

bool WereadClient::hasValidCookie() const {
    return cookies_.count("wr_vid") && cookies_.count("wr_skey");
}

void WereadClient::absorbSetCookie(const HttpResponse& resp) {
    auto it = resp.headers.find("set-cookie");
    if (it == resp.headers.end()) return;
    // 诊断：打印收到的原始 set-cookie（登录后验证服务器是否下发 wr_skey/wr_rt）
    ESP_LOGI(TAG, "set-cookie 收到: %.240s", it->second.c_str());
    // 逐行解析 name=value（只取第一个分号前的部分）
    const std::string& all = it->second;
    size_t start = 0;
    while (start <= all.size()) {
        size_t nl = all.find('\n', start);
        if (nl == std::string::npos) nl = all.size();
        std::string line = all.substr(start, nl - start);
        size_t eq = line.find('=');
        size_t semi = line.find(';');
        if (eq != std::string::npos && eq > 0) {
            std::string name = line.substr(0, eq);
            // trim
            while (!name.empty() && (name.back() == ' ' || name.back() == '\t')) name.pop_back();
            while (!name.empty() && (name.front() == ' ' || name.front() == '\t')) name.erase(0, 1);
            std::string val;
            if (semi != std::string::npos && semi > eq) val = line.substr(eq + 1, semi - eq - 1);
            else val = line.substr(eq + 1);
            while (!val.empty() && (val.back() == ' ' || val.back() == '\t')) val.pop_back();
            while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) val.erase(0, 1);
            // 只关心 weread 的会话字段，避免膨胀
            if (name.size() >= 3 && name.compare(0, 3, "wr_") == 0) {
                // 会话关键字段被服务器轮换（值变化）→ 置脏，主循环定期写回配置文件
                if ((name == "wr_skey" || name == "wr_rt" || name == "wr_vid") &&
                    cookies_.count(name) && cookies_[name] != val) {
                    cookies_dirty_ = true;
                }
                setCookie(name, val);
            }
        }
        start = nl + 1;
    }
}

// ---- keep-alive 会话 ----
// host 固定 weread.qq.com：书架/目录/详情/评论/续期/进度上传全走这一条 TLS 连接，
// 握手从每请求 1 次（1-2s）降到整会话 1 次；失败 close 重建重试一次。

// 会话响应缓冲：event_handler 的 user_data 在 init 时绑定无法按请求改，
// 故用静态固定缓冲，每次请求前重置，成功后转所有权。
static RespBuf g_sess_rb;

bool WereadClient::sessOpen_() {
    if (sess_) return true;
    esp_http_client_config_t cfg = {};
    cfg.host = "weread.qq.com";
    cfg.path = "/";
    cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;
    cfg.keep_alive_enable = true;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &g_sess_rb;
    sess_ = esp_http_client_init(&cfg);
    return sess_ != nullptr;
}

void WereadClient::sessClose_() {
    if (sess_) { esp_http_client_cleanup(sess_); sess_ = nullptr; }
}

void WereadClient::housekeeping() {
    // 闲置 5s 即关闭：weread 服务器会悄悄断开闲置 keep-alive 连接，
    // 复用时读超时要干等 20s（表现为详情/阅读页卡死）。缩短闲置窗口，
    // 让用户操作停顿后的首个请求都走全新连接，代价只是多一次 TLS 握手。
    if (sess_ && sess_in_use_ == 0 && millis_now() - sess_last_use_ > 5000) {
        ESP_LOGI(TAG, "会话闲置 5s，关闭释放内存");
        sessClose_();
    }
}

// 会话路径单次请求（不 retry）；path 为相对路径（如 "/web/shelf/sync"）
HttpResponse WereadClient::requestSess_(const std::string& method, const std::string& path,
                                        const std::string* body, const std::string& referer,
                                        const std::string& contentType) {
    HttpResponse resp;
    struct UseGuard { int& c; UseGuard(int& c_) : c(c_) { c++; } ~UseGuard() { c--; } } guard(sess_in_use_);

    if (!sessOpen_()) { resp.error = "sess init failed"; resp.status = -1; return resp; }

    // 重置会话缓冲（headers/PSRAM 指针指到当次 resp）
    g_sess_rb.body = &resp.body;
    g_sess_rb.headers = &resp.headers;
    g_sess_rb.pslen = 0;
    g_sess_rb.overflow = false;
    if (!g_sess_rb.psbuf) {
        g_sess_rb.psbuf = (char*)heap_caps_malloc(HTTP_BUF_INIT, MALLOC_CAP_SPIRAM);
        if (!g_sess_rb.psbuf) { resp.error = "PSRAM 分配失败"; resp.status = -1; return resp; }
        g_sess_rb.pscap = HTTP_BUF_INIT;
    }

    esp_http_client_set_url(sess_, path.c_str());
    esp_http_client_set_method(sess_, method == "POST" ? HTTP_METHOD_POST : HTTP_METHOD_GET);
    esp_http_client_set_header(sess_, "User-Agent", WEREAD_USER_AGENT);
    esp_http_client_set_header(sess_, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(sess_, "Origin", "https://weread.qq.com");
    esp_http_client_set_header(sess_, "Referer", referer.c_str());
    std::string cookie = cookieHeader();
    if (!cookie.empty()) esp_http_client_set_header(sess_, "Cookie", cookie.c_str());
    if (body) {
        esp_http_client_set_header(sess_, "Content-Type", contentType.c_str());
        esp_http_client_set_post_field(sess_, body->data(), body->size());
    } else {
        esp_http_client_set_post_field(sess_, nullptr, 0); // 清掉上次 POST body
    }

    esp_err_t err = esp_http_client_perform(sess_);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(sess_);
    } else {
        resp.error = esp_err_to_name(err);
        resp.status = -1;
    }
    sess_last_use_ = millis_now();

    if (g_sess_rb.overflow) {
        ESP_LOGW(TAG, "响应超 %u KB 上限被截断 path=%s",
                 (unsigned)(g_sess_rb.pscap / 1024), path.c_str());
        g_sess_rb.overflow = false;
        resp.error = "response too large";
        resp.status = -1;
    }
    // 完整性校验：比较 Content-Length 与实际接收长度
    // chunked 传输时 content_length 返回 -1，跳过校验
    if (resp.status > 0) {
        int64_t clen = esp_http_client_get_content_length(sess_);
        if (clen > 0 && (int64_t)g_sess_rb.pslen < clen) {
            ESP_LOGW(TAG, "响应不完整: 收到 %u / 期望 %lld bytes, path=%s",
                     (unsigned)g_sess_rb.pslen, (long long)clen, path.c_str());
            resp.error = "incomplete response";
            resp.status = -1;
        }
    }
    // 成功收到数据：转所有权给 resp（g_sess_rb.psbuf 置空，下次请求重新分配）
    if (resp.status > 0 && g_sess_rb.psbuf && g_sess_rb.pslen > 0) {
        if (g_sess_rb.pslen < g_sess_rb.pscap) g_sess_rb.psbuf[g_sess_rb.pslen] = '\0';
        resp.psbody = g_sess_rb.psbuf;
        resp.pslen = g_sess_rb.pslen;
        g_sess_rb.psbuf = nullptr;
        g_sess_rb.pscap = 0;
    }
    absorbSetCookie(resp);
    return resp;
}

// 一次性独立 client（fallback：异 host 如 i.weread.qq.com，或会话重建后仍失败的兜底）
HttpResponse WereadClient::requestOnce_(const std::string& method, const std::string& url,
                                        const std::string* body, const std::string& referer,
                                        const std::string& contentType) {
    HttpResponse resp;
    RespBuf rb;
    rb.body = &resp.body;
    rb.headers = &resp.headers;

    char* psbuf = (char*)heap_caps_malloc(HTTP_BUF_INIT, MALLOC_CAP_SPIRAM);
    if (psbuf) { rb.psbuf = psbuf; rb.pscap = HTTP_BUF_INIT; rb.pslen = 0; }

    esp_http_client_config_t cfg = {};
    cfg.url = url.c_str();
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &rb;
    cfg.timeout_ms = HTTP_TIMEOUT_MS;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 2048;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) { resp.error = "init failed"; if (psbuf) heap_caps_free(psbuf); return resp; }

    if (method == "POST") esp_http_client_set_method(client, HTTP_METHOD_POST);
    else esp_http_client_set_method(client, HTTP_METHOD_GET);

    esp_http_client_set_header(client, "User-Agent", WEREAD_USER_AGENT);
    esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
    esp_http_client_set_header(client, "Origin", "https://weread.qq.com");
    esp_http_client_set_header(client, "Referer", referer.c_str());
    std::string cookie = cookieHeader();
    if (!cookie.empty()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
    if (body) {
        esp_http_client_set_header(client, "Content-Type", contentType.c_str());
        esp_http_client_set_post_field(client, body->data(), body->size());
    }

    esp_err_t err = esp_http_client_perform(client);
    if (err == ESP_OK) {
        resp.status = esp_http_client_get_status_code(client);
    } else {
        resp.error = esp_err_to_name(err);
        resp.status = -1;
    }
    // 完整性校验：比较 Content-Length 与实际接收长度
    // （需在 cleanup 前检查，cleanup 后 client 已释放）
    // chunked 传输时 content_length 返回 -1，跳过校验
    if (resp.status > 0) {
        int64_t clen = esp_http_client_get_content_length(client);
        if (clen > 0 && (int64_t)rb.pslen < clen) {
            ESP_LOGW(TAG, "响应不完整: 收到 %u / 期望 %lld bytes, url=%s",
                     (unsigned)rb.pslen, (long long)clen, url.c_str());
            resp.error = "incomplete response";
            resp.status = -1;
        }
    }
    esp_http_client_cleanup(client);

    if (rb.overflow) {
        ESP_LOGW(TAG, "响应超 %u KB 上限被截断 url=%s",
                 (unsigned)(rb.pscap / 1024), url.c_str());
        resp.error = "response too large";
        resp.status = -1;
    }
    if (rb.psbuf && rb.pslen > 0) {
        if (rb.pslen < rb.pscap) rb.psbuf[rb.pslen] = '\0';
        resp.psbody = rb.psbuf;
        resp.pslen = rb.pslen;
    } else if (rb.psbuf) {
        heap_caps_free(rb.psbuf);
    }
    absorbSetCookie(resp);
    return resp;
}

HttpResponse WereadClient::request(const std::string& method, const std::string& url,
                                   const std::string* body, const std::string& referer,
                                   const std::string& contentType) {
    // 自动路由：weread.qq.com 走 keep-alive 会话；异 host 走一次性 client
    static const char* WEB_PREFIX = "https://weread.qq.com";
    bool use_sess = url.compare(0, strlen(WEB_PREFIX), WEB_PREFIX) == 0;

    auto do_once = [&](const std::string& m, const std::string& u, const std::string* b,
                       const std::string& ref, const std::string& ct) -> HttpResponse {
        if (use_sess) {
            std::string path = u.substr(strlen(WEB_PREFIX)); // "/web/..." 部分
            if (path.empty()) path = "/";
            HttpResponse r = requestSess_(m, path, b, ref, ct);
            if (r.status == -1) { // 连接级失败：丢弃重建再试一次
                ESP_LOGW(TAG, "会话请求失败，重建连接重试");
                sessClose_();
                r = requestSess_(m, path, b, ref, ct);
            }
            return r;
        }
        return requestOnce_(m, u, b, ref, ct);
    };

    HttpResponse resp = do_once(method, url, body, referer, contentType);

    // 自动续期：wr_skey 短效（几小时过期），任何请求吃到 -2012 就用 wr_rt 续期并重试一次。
    if (resp.ok() && !renewing_ && hasValidCookie() && resp.length() < 2048 &&
        find_sub(resp.data(), resp.length(), "-2012", 5) >= 0) {
        if (!last_renew_fail_ || millis_now() - last_renew_fail_ > 600000) {
            ESP_LOGI(TAG, "请求返回 -2012，自动续期并重试");
            if (tryRenew()) { // tryRenew 内部自置 renewing_ 防递归
                resp = do_once(method, url, body, referer, contentType); // 新 cookie 重发
            }
        }
    }
    return resp;
}

HttpResponse WereadClient::get(const std::string& url, const std::string& referer) {
    return request("GET", url, nullptr, referer, "");
}

HttpResponse WereadClient::postJson(const std::string& url, const std::string& jsonBody,
                                    const std::string& referer) {
    return request("POST", url, &jsonBody, referer, "application/json;charset=UTF-8");
}

// 续期：wr_skey 是短效令牌（几小时过期），用 wr_rt 调 renewal 换新；成功返回 true 并落盘
bool WereadClient::tryRenew() {
    renewing_ = true; // 防 request 层对 renewal 响应再触发自动续期（递归）
    // 诊断：确认发续期请求时 wr_rt/wr_skey 是否已加载（判断 -2013 是否因缺 rt）
    ESP_LOGI(TAG, "renewal 尝试 rt_len=%u skey_len=%u",
             (unsigned)getCookie("wr_rt").size(), (unsigned)getCookie("wr_skey").size());
    std::string body = "{\"rq\":\"%2Fweb%2Fbook%2Fread\",\"ql\":false}";
    HttpResponse r = postJson(std::string(WEREAD_HOST_WEB) + "/web/login/renewal", body);
    renewing_ = false;
    if (!r.ok()) {
        ESP_LOGW(TAG, "renewal 传输失败 status=%d err=%s（30s 后可再试）", r.status, r.error.c_str());
        last_renew_fail_ = millis_now() - 570000; // 相当于 30s 后解除防抖（600s-570s）
        return false;
    }
    // 解析响应：errcode 非 0 / 无 succ 即失败（r 可能是 PSRAM，用 data()/length()）
    int ec = 0, succ = 0;
    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (root) {
        cJSON* ec_item = cJSON_GetObjectItem(root, "errcode");
        if (!ec_item || cJSON_IsNull(ec_item)) ec_item = cJSON_GetObjectItem(root, "errCode");
        if (ec_item && cJSON_IsNumber(ec_item)) ec = ec_item->valueint;
        cJSON* succ_item = cJSON_GetObjectItem(root, "succ");
        if (succ_item && cJSON_IsNumber(succ_item)) succ = succ_item->valueint;
        cJSON_Delete(root);
    }
    if (ec != 0 || !succ) {
        ESP_LOGW(TAG, "renewal 被拒 errcode=%d（wr_rt 已失效，需重新扫码）", ec);
        last_renew_fail_ = millis_now(); // rt 死亡：10 分钟防抖
        rt_dead_ = true;
        return false;
    }
    // 新 wr_skey 已由 absorbSetCookie 吸收进内存，落盘
    ESP_LOGI(TAG, "renewal 成功，新令牌已生效");
    saveCookiesToConfig();
    clearCookiesDirty();
    return true;
}

} // namespace eego
