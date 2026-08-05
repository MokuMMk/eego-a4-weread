// config.h — 应用配置管理（WiFi 列表 + 版式参数）
// 管理 /sdcard/weread/config.json，使用 cJSON 替代 ArduinoJson。
// config.json 同时被配网门户写入（wifi_list 数组），保存时保留未知字段
//（如 wr_vid/wr_skey/wr_rt 等 cookie），避免覆盖其它组件写入的数据。
#pragma once
#include <stdbool.h>
#include <stdint.h>

namespace eego {

// 单个 WiFi 账号（SSID 最长 32 字节 + '\0'；WPA 密码最长 64 字节 + '\0'）
struct WifiAccount {
    char ssid[33];
    char password[65];
};

// 应用配置（纯 POD，可 memcmp 比较）
struct AppConfig {
    WifiAccount wifiList[5];   // 多 WiFi 账号（配网门户可追加）
    int wifiCount;             // 实际账号数（0..5）
    char fontPath[128];        // 正文字体路径（空 = 默认字体）
    int lineHeight;            // 行距倍数（1.0x = 100）
    int paragraphSpacing;      // 段距档位 (0=无 1=行距/4 2=行距/2)
    int firstLineIndent;       // 首行缩进档位 (0=无 1=1字符 2=2字符)
    bool showImages;           // 插图开关（全局；按书开关在 imgoff.json）
    int fontSize;              // 正文字号
};

class Config {
public:
    static Config& instance();

    bool load();               // 从 /sdcard/weread/config.json 加载
    bool save();               // 保存到 /sdcard/weread/config.json（保留未知字段）
    bool saveIfChanged();      // 与磁盘上的配置比较，不同才保存

    const AppConfig& get() const { return config_; }
    AppConfig& mutableGet() { return config_; }

private:
    Config() = default;
    bool loadInto(AppConfig& out);  // 解析 JSON 到指定 struct（内部复用）
    AppConfig config_ = {};
    bool loaded_ = false;
    bool dirty_ = false;
};

}  // namespace eego
