// EEGO A4 SD 卡管理 — 纯 ESP-IDF（SDSPI + FATFS via VFS）
// 独立 SPI 总线（SPI2_HOST），与显示 EPD-SPI 分离
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <esp_err.h>
#include "sdmmc_cmd.h"  // sdmmc_card_t

namespace eego {

class SdCard {
public:
    static SdCard& instance();  // 单例

    esp_err_t mount();     // 挂载 SD 卡到 /sdcard
    void unmount();
    bool ready() const { return mounted_; }

    // 文件操作（路径相对于 /sdcard 根，支持 UTF-8 中文长名）
    bool exists(const char* path);
    bool mkdir(const char* path);
    bool remove(const char* path);
    bool rename(const char* from, const char* to);

    // 列目录
    struct DirEntry {
        char name[256];
        bool isDir;
        size_t size;
    };
    int listDir(const char* path, DirEntry* entries, int maxEntries);

    // 文件读写（封装 FILE*）
    FILE* open(const char* path, const char* mode);
    size_t readFile(const char* path, uint8_t* buffer, size_t maxLen);  // 一次性读整个文件
    bool writeFile(const char* path, const uint8_t* data, size_t len);  // 一次性写

    size_t fileSize(const char* path);

private:
    SdCard() = default;
    void makeFullPath(const char* path, char* out, size_t outLen) const;
    esp_err_t mountOnce(uint32_t freqKhz);  // 单次挂载尝试
    bool mounted_ = false;
    sdmmc_card_t* card_ = nullptr;
};

}  // namespace eego
