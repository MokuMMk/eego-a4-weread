// config.cpp — 应用配置管理实现（cJSON）
//
// config.json 结构（与配网门户兼容）：
// {
//   "wifi_list": [{"ssid":"...","pass":"..."}, ...],
//   "wifi_ssid": "...",  // 兼容老格式（= wifi_list[0].ssid）
//   "wifi_pass": "...",  // 兼容老格式
//   "font": "/font/xxx.eefont",
//   "line_height": 100,    // 行距倍数 1.0x=100
//   "para_gap": 1,         // 段距档位 0/1/2
//   "para_indent": 2,      // 首行缩进 0/1/2
//   "show_images": true,
//   "font_size": 26,
//   "wr_vid": "...",       // cookie（其它组件写入，保存时保留）
//   "wr_skey": "...",
//   "wr_rt": "...",
//   "api_key": ""
// }
#include "config.h"
#include "storage.h"   // SD_CONFIG_PATH
#include "sd_card.h"

#include <cstring>
#include <string>

#include "cJSON.h"
#include "esp_log.h"

namespace eego {

static const char* TAG = "eego.cfg";

Config& Config::instance() {
    static Config inst;
    return inst;
}

// ---- cJSON 取值辅助 ----
static void parseWifiList(cJSON* root, AppConfig& cfg) {
    cfg.wifiCount = 0;
    cJSON* arr = cJSON_GetObjectItem(root, "wifi_list");
    if (cJSON_IsArray(arr)) {
        cJSON* ap = nullptr;
        cJSON_ArrayForEach(ap, arr) {
            if (cfg.wifiCount >= 5) break;
            const char* ssid = cJSON_GetStringValue(cJSON_GetObjectItem(ap, "ssid"));
            const char* pass = cJSON_GetStringValue(cJSON_GetObjectItem(ap, "pass"));
            if (ssid && ssid[0]) {
                WifiAccount& w = cfg.wifiList[cfg.wifiCount++];
                strncpy(w.ssid, ssid, sizeof(w.ssid) - 1);
                w.ssid[sizeof(w.ssid) - 1] = '\0';
                if (pass) {
                    strncpy(w.password, pass, sizeof(w.password) - 1);
                    w.password[sizeof(w.password) - 1] = '\0';
                } else {
                    w.password[0] = '\0';
                }
            }
        }
    }
    // 兼容老格式单账号：wifi_ssid/wifi_pass 并入列表（去重）
    const char* legacySsid = cJSON_GetStringValue(cJSON_GetObjectItem(root, "wifi_ssid"));
    if (legacySsid && legacySsid[0]) {
        bool found = false;
        for (int i = 0; i < cfg.wifiCount; i++) {
            if (strcmp(cfg.wifiList[i].ssid, legacySsid) == 0) { found = true; break; }
        }
        if (!found && cfg.wifiCount < 5) {
            WifiAccount& w = cfg.wifiList[cfg.wifiCount++];
            strncpy(w.ssid, legacySsid, sizeof(w.ssid) - 1);
            w.ssid[sizeof(w.ssid) - 1] = '\0';
            const char* legacyPass = cJSON_GetStringValue(cJSON_GetObjectItem(root, "wifi_pass"));
            if (legacyPass) {
                strncpy(w.password, legacyPass, sizeof(w.password) - 1);
                w.password[sizeof(w.password) - 1] = '\0';
            } else {
                w.password[0] = '\0';
            }
        }
    }
}

static void copyStr(const char* src, char* dst, size_t dstLen) {
    if (src && dstLen > 0) {
        strncpy(dst, src, dstLen - 1);
        dst[dstLen - 1] = '\0';
    } else if (dstLen > 0) {
        dst[0] = '\0';
    }
}

// cJSON 字段取整数（NULL 安全，缺失返回默认值）
static int jInt(cJSON* obj, const char* key, int def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!item || !cJSON_IsNumber(item)) return def;
    return item->valueint;
}

bool Config::loadInto(AppConfig& out) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) {
        ESP_LOGW(TAG, "SD not ready, cannot load config");
        return false;
    }
    if (!sd.exists(SD_CONFIG_PATH)) {
        ESP_LOGI(TAG, "config.json not found, using defaults");
        return false;
    }

    // 读文件到内存
    FILE* f = sd.open(SD_CONFIG_PATH, "rb");
    if (!f) return false;
    std::string buf;
    char chunk[512];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
        buf.append(chunk, n);
    }
    fclose(f);
    if (buf.empty()) return false;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) {
        ESP_LOGW(TAG, "config.json parse failed");
        return false;
    }

    parseWifiList(root, out);

    const char* font = cJSON_GetStringValue(cJSON_GetObjectItem(root, "font"));
    copyStr(font, out.fontPath, sizeof(out.fontPath));

    out.lineHeight        = jInt(root, "line_height", 100);
    out.paragraphSpacing  = jInt(root, "para_gap", 1);
    out.firstLineIndent   = jInt(root, "para_indent", 2);
    // show_images 缺失时默认开图
    {
        cJSON* si = cJSON_GetObjectItem(root, "show_images");
        out.showImages = si ? cJSON_IsTrue(si) : true;
    }
    out.fontSize          = jInt(root, "font_size", 26);

    // 前光亮度/色温（0-100）— 默认关闭（亮度 0），用户手动调节后保存
    out.brightness        = jInt(root, "brightness", 0);
    out.warmth            = jInt(root, "warmth", 50);
    if (out.brightness < 0) out.brightness = 0;
    if (out.brightness > 100) out.brightness = 100;
    if (out.warmth < 0) out.warmth = 0;
    if (out.warmth > 100) out.warmth = 100;

    cJSON_Delete(root);
    return true;
}

bool Config::load() {
    // 先填默认值
    config_ = {};
    config_.lineHeight       = 100;   // 1.0x
    config_.paragraphSpacing = 1;     // 标准段距
    config_.firstLineIndent  = 2;     // 2 字符缩进
    config_.showImages       = true;
    config_.fontSize         = 26;
    config_.brightness       = 0;
    config_.warmth           = 50;

    if (!loadInto(config_)) {
        // 文件不存在/解析失败：保留默认值，标记已加载
        loaded_ = true;
        return false;
    }
    loaded_ = true;
    ESP_LOGI(TAG, "config loaded: wifi=%d font=%s lh=%d pg=%d pi=%d img=%d fs=%d",
             config_.wifiCount, config_.fontPath, config_.lineHeight,
             config_.paragraphSpacing, config_.firstLineIndent,
             config_.showImages, config_.fontSize);
    return true;
}

// 写入字段辅助：先删旧 key 再添加，避免重复
static void setStr(cJSON* obj, const char* key, const char* val) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddStringToObject(obj, key, val ? val : "");
}
static void setNum(cJSON* obj, const char* key, double val) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddNumberToObject(obj, key, val);
}
static void setBool(cJSON* obj, const char* key, bool val) {
    cJSON_DeleteItemFromObject(obj, key);
    cJSON_AddBoolToObject(obj, key, val);
}

bool Config::save() {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) {
        ESP_LOGW(TAG, "SD not ready, cannot save config");
        return false;
    }

    // 读已有配置（保留未知字段：cookie、api_key 等）
    cJSON* root = nullptr;
    if (sd.exists(SD_CONFIG_PATH)) {
        FILE* f = sd.open(SD_CONFIG_PATH, "rb");
        if (f) {
            std::string buf;
            char chunk[512];
            size_t n;
            while ((n = fread(chunk, 1, sizeof(chunk), f)) > 0) {
                buf.append(chunk, n);
            }
            fclose(f);
            if (!buf.empty()) root = cJSON_Parse(buf.c_str());
        }
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) return false;

    // wifi_list 数组
    cJSON* wifiArr = cJSON_CreateArray();
    for (int i = 0; i < config_.wifiCount && i < 5; i++) {
        cJSON* ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", config_.wifiList[i].ssid);
        cJSON_AddStringToObject(ap, "pass", config_.wifiList[i].password);
        cJSON_AddItemToArray(wifiArr, ap);
    }
    cJSON_DeleteItemFromObject(root, "wifi_list");
    cJSON_AddItemToObject(root, "wifi_list", wifiArr);

    // 兼容老格式：wifi_ssid/wifi_pass = 第一个账号
    if (config_.wifiCount > 0) {
        setStr(root, "wifi_ssid", config_.wifiList[0].ssid);
        setStr(root, "wifi_pass", config_.wifiList[0].password);
    }

    // 版式参数
    setStr(root, "font",        config_.fontPath);
    setNum(root, "line_height", config_.lineHeight);
    setNum(root, "para_gap",    config_.paragraphSpacing);
    setNum(root, "para_indent", config_.firstLineIndent);
    setBool(root, "show_images", config_.showImages);
    setNum(root, "font_size",   config_.fontSize);

    // 前光亮度/色温
    setNum(root, "brightness", config_.brightness);
    setNum(root, "warmth",     config_.warmth);

    // 序列化并写入
    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) {
        ESP_LOGE(TAG, "cJSON_Print failed");
        return false;
    }

    sd.mkdir(SD_WEREAD_DIR);
    size_t len = strlen(str);
    bool ok = sd.writeFile(SD_CONFIG_PATH, (const uint8_t*)str, len);
    free(str);

    if (ok) {
        dirty_ = false;
        loaded_ = true;
        ESP_LOGI(TAG, "config saved (%d bytes)", (int)len);
    } else {
        ESP_LOGE(TAG, "write config.json failed");
    }
    return ok;
}

bool Config::saveIfChanged() {
    if (!loaded_) load();
    // 从磁盘重新读一份比较
    AppConfig disk = {};
    bool existed = loadInto(disk);
    if (existed && memcmp(&disk, &config_, sizeof(AppConfig)) == 0) {
        return true;  // 无变化
    }
    return save();
}

}  // namespace eego
