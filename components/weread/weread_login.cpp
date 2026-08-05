// weread_login.cpp — 实现
// 纯 ESP-IDF：cJSON 解析响应，vTaskDelay 替代 delay，ESP_LOG 替代 Serial。
#include "weread_login.h"
#include "weread_client.h"
#include "storage.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>

namespace weread_login {

using eego::WR;
using eego::HttpResponse;
// WEREAD_HOST_WEB 是宏，自动可见，无需 using

static const char* TAG = "eego.login";

static int64_t millis_now() { return esp_timer_get_time() / 1000; }

static void debug_log(const std::string& s) {
    eego::Storage::instance().log("%s", s.c_str());
}

// 从 cJSON 取字符串（webLoginVid/uid 等可能是数字而非字符串）
static std::string jstr(cJSON* item) {
    if (!item) return "";
    if (cJSON_IsString(item)) return item->valuestring ? std::string(item->valuestring) : std::string();
    if (cJSON_IsNumber(item)) return std::to_string(item->valueint);
    return "";
}

// 响应开头（数据可能在 PSRAM，按长度拼，不依赖 \0 结尾）
static std::string resp_head(const HttpResponse& r, size_t n = 160) {
    std::string s = "head=";
    s.append(r.data(), r.length() < n ? r.length() : n);
    return s;
}

// cJSON 字段取值：优先 data 对象，缺失回退顶层
static cJSON* field(cJSON* root, cJSON* data, const char* key) {
    cJSON* v = nullptr;
    if (data) v = cJSON_GetObjectItem(data, key);
    if (!v || cJSON_IsNull(v)) v = cJSON_GetObjectItem(root, key);
    return v;
}

bool run(std::function<void(const std::string& url, const std::string& status)> render_qr,
         std::function<void(const std::string&, const std::string&, const std::string&)> msg,
         unsigned long timeout_ms) {

    // ---- 1. 获取登录 uid ----
    HttpResponse r = WR.get(std::string(WEREAD_HOST_WEB) + "/api/auth/getLoginUid");
    if (!r.ok()) {
        ESP_LOGE(TAG, "getLoginUid 失败 status=%d err=%s", r.status, r.error.c_str());
        debug_log("getLoginUid 失败 status=" + std::to_string(r.status) + " err=" + r.error);
        // status=-1 是传输层失败（DNS/TCP/TLS），>0 是服务器拒绝
        msg("登录失败", r.status > 0 ? std::string("服务器繁忙 HTTP ") + std::to_string(r.status)
                                     : "网络不通请检查网络", "点按重试");
        return false;
    }
    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (!root) {
        ESP_LOGE(TAG, "getLoginUid JSON 解析失败");
        debug_log("getLoginUid 解析失败 " + resp_head(r));
        msg("登录失败", "服务器返回异常", "点按重试");
        return false;
    }
    std::string uid = jstr(cJSON_GetObjectItem(root, "uid"));
    if (uid.empty()) {
        cJSON* data = cJSON_GetObjectItem(root, "data");
        if (data) uid = jstr(cJSON_GetObjectItem(data, "uid"));
    }
    if (uid.empty()) {
        ESP_LOGE(TAG, "getLoginUid 响应无 uid: %.160s", r.data());
        debug_log("getLoginUid 无 uid " + resp_head(r));
        cJSON_Delete(root);
        msg("登录失败", "服务器返回异常", "点按重试");
        return false;
    }
    cJSON_Delete(root);
    ESP_LOGI(TAG, "uid=%s", uid.c_str());

    // ---- 2. 显示二维码，等待扫码 ----
    render_qr(std::string(WEREAD_HOST_WEB) + "/web/confirm?uid=" + uid, "用微信/微信读书扫码");

    // ---- 3. 每 2 秒轮询登录状态 ----
    int64_t start = millis_now();
    bool ok_login = false;
    std::string vid, token;
    while (millis_now() - start < (int64_t)timeout_ms) {
        HttpResponse pr = WR.get(std::string(WEREAD_HOST_WEB) + "/api/auth/getLoginInfo?uid=" + uid + "&otp=");
        if (!pr.ok()) {
            ESP_LOGW(TAG, "轮询 HTTP 失败 status=%d err=%s，继续", pr.status, pr.error.c_str());
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        cJSON* pdoc = cJSON_ParseWithLength(pr.data(), pr.length());
        if (!pdoc) {
            ESP_LOGW(TAG, "轮询 JSON 解析失败，继续");
            vTaskDelay(pdMS_TO_TICKS(2000));
            continue;
        }
        cJSON* data = cJSON_GetObjectItem(pdoc, "data");

        bool succeed = false;
        cJSON* succ_item = field(pdoc, data, "succeed");
        if (succ_item && cJSON_IsTrue(succ_item)) succeed = true;

        std::string pv = jstr(field(pdoc, data, "webLoginVid"));
        if (pv.empty()) pv = jstr(field(pdoc, data, "vid"));
        if (pv.empty()) pv = jstr(field(pdoc, data, "userVid"));
        if (pv.empty()) pv = jstr(field(pdoc, data, "user_vid"));
        std::string ptok = jstr(field(pdoc, data, "accessToken"));
        std::string logic = jstr(field(pdoc, data, "logicCode"));

        ESP_LOGI(TAG, "轮询 succeed=%d vid=%s token=%s logic=%s",
                 succeed, pv.empty() ? "无" : "有", ptok.empty() ? "无" : "有", logic.c_str());

        if (succeed && !pv.empty() && !ptok.empty()) {
            vid = pv; token = ptok; ok_login = true;
            cJSON_Delete(pdoc);
            break;
        }
        if (logic == "NEED_OTP" || logic == "OTP_EXPIRED" || logic == "OTP_NOT_MATCH") {
            msg("需要手机 OTP 验证", "暂不支持", logic);
            cJSON_Delete(pdoc);
            return false;
        }
        if (!logic.empty() && logic != "LOGIN_TIMEOUT") {
            msg("登录失败", logic, "点按重试");
            cJSON_Delete(pdoc);
            return false;
        }
        cJSON_Delete(pdoc);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }

    if (!ok_login) {
        msg("扫码超时", "点按屏幕重试", "");
        return false;
    }

    // ---- 4. 收尾：保存 cookie ----
    // 真正的 wr_skey/wr_vid 在 getLoginInfo 响应的 set-cookie 里，
    // WereadClient 已自动吸收进 cookie jar；只在缺失时用 accessToken/vid 兜底
    if (WR.getCookie("wr_skey").empty()) WR.setCookie("wr_skey", token);
    if (WR.getCookie("wr_vid").empty())  WR.setCookie("wr_vid", vid);
    WR.saveCookiesToConfig(); // 把 wr_* 写回配置文件并保留其它字段
    // 诊断：登录完成后 jar 里的 cookie 情况（判断是否拿到了服务器新下发的 wr_skey/wr_rt）
    ESP_LOGI(TAG, "登录后 jar: vid_len=%u skey_len=%u rt_len=%u",
             (unsigned)WR.getCookie("wr_vid").size(),
             (unsigned)WR.getCookie("wr_skey").size(),
             (unsigned)WR.getCookie("wr_rt").size());
    msg("登录成功", "正在加载书架...", "");
    return true;
}

} // namespace weread_login
