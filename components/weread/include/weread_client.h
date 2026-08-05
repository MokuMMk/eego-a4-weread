// weread_client.h — HTTP/TLS 客户端 + cookie 管理（基于 esp_http_client）
// 纯 ESP-IDF：std::string 替代 Arduino String，esp_http_client 替代 Arduino HTTPClient，
// esp_crt_bundle 替代手动证书，cookie 持久化到 SD 卡 config.json。
#pragma once
#include <string>
#include <vector>
#include <map>
#include <utility>

#include "esp_http_client.h"
#include "esp_err.h"

namespace eego {

// 网络常量
#define WEREAD_HOST_I   "https://i.weread.qq.com"
#define WEREAD_HOST_WEB "https://weread.qq.com"
#define HTTP_TIMEOUT_MS 20000

// 与 weread.koplugin 一致的 UA（web_app_id / 签名会用到）
extern const char* WEREAD_USER_AGENT;

struct HttpResponse {
    int status = 0;
    std::string body;
    std::map<std::string,std::string> headers; // 小写 key
    std::string error;
    // 大响应走 PSRAM（std::string 超大响应时优先用 PSRAM 缓冲）；非空时以 psbody/pslen 为准
    char* psbody = nullptr;
    size_t pslen = 0;
    // 拥有 psbody 所有权：移动语义 + 析构释放，禁止拷贝（防 double-free）
    HttpResponse() = default;
    ~HttpResponse();
    HttpResponse(HttpResponse&& o) noexcept;
    HttpResponse& operator=(HttpResponse&& o) noexcept;
    HttpResponse(const HttpResponse&) = delete;
    HttpResponse& operator=(const HttpResponse&) = delete;
    bool ok() const { return status >= 200 && status < 300; }
    // 有效数据的指针和长度（优先 PSRAM）
    const char* data() const { return psbody ? psbody : body.data(); }
    size_t length() const { return psbody ? pslen : body.size(); }
};

class WereadClient {
public:
    WereadClient() = default;
    ~WereadClient();

    // 从 SD 卡 config.json 加载 weread cookie（wr_vid/wr_skey/wr_rt/api_key）
    bool loadConfig();

    // cookie 操作
    void setCookie(const std::string& name, const std::string& value);
    std::string getCookie(const std::string& name) const;
    std::string cookieHeader() const;
    bool hasValidCookie() const;
    bool saveCookiesToConfig(); // 把当前 wr_* cookie 写回配置文件（保留 wifi 等其它字段）
    // cookie 被服务器轮换后置脏，主循环定期落盘（否则重启后旧 cookie 失效要重扫）
    bool cookiesDirty() const { return cookies_dirty_; }
    void clearCookiesDirty() { cookies_dirty_ = false; }
    // wr_rt 已被服务器判死（renewal 返回 -2012）：只能重新扫码登录
    bool rtDead() const { return rt_dead_; }

    // 通用请求（自动带 cookie、UA、referer）
    HttpResponse get(const std::string& url, const std::string& referer = "https://weread.qq.com/");
    HttpResponse postJson(const std::string& url, const std::string& jsonBody,
                          const std::string& referer = "https://weread.qq.com/");

    // HEAD 请求（获取响应头，不下载 body）
    HttpResponse head(const std::string& url);

    // 用 wr_rt 续期 wr_skey（短效令牌）；成功自动落盘
    bool tryRenew();

    // 会话闲置关闭（主循环周期调用）：释放 keep-alive TLS 占用的 DRAM
    void housekeeping();

    // 配置值（从配置文件读入）
    std::string cfg_api_key;

private:
    std::map<std::string,std::string> cookies_;
    bool cookies_dirty_ = false;    // wr_skey/wr_rt 被 set-cookie 轮换后置真
    bool renewing_ = false;         // tryRenew 进行中（防 request 层自动续期递归）
    bool rt_dead_ = false;          // renewal 被服务器拒绝（wr_rt 已死，只能重扫码）
    int64_t last_renew_fail_ = 0;   // 上次续期失败时间（传输失败 30s、rt 死亡 10 分钟防抖）

    // ---- keep-alive 会话 ----
    // host 固定 weread.qq.com，open 一次后 set_url 换路径复用；
    // perform 失败 close 重建重试一次；in_use RAII 计数 + last_use 闲置 30s 由 housekeeping 关。
    esp_http_client_handle_t sess_ = nullptr;
    int64_t sess_last_use_ = 0;
    int sess_in_use_ = 0;  // RAII 计数（单线程网络任务中使用，无需原子）
    bool sessOpen_();
    void sessClose_();
    HttpResponse requestSess_(const std::string& method, const std::string& path,
                              const std::string* body, const std::string& referer,
                              const std::string& contentType);
    HttpResponse requestOnce_(const std::string& method, const std::string& url,
                              const std::string* body, const std::string& referer,
                              const std::string& contentType);

    HttpResponse request(const std::string& method, const std::string& url,
                         const std::string* body, const std::string& referer,
                         const std::string& contentType);
    void absorbSetCookie(const HttpResponse& resp);
};

// 全局实例（替代原始 Arduino 工程的 `WereadClient WR;`）
extern WereadClient WR;

} // namespace eego
