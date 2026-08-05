// EEGO A4 SD 卡管理实现 — 纯 ESP-IDF
//
// 使用 SDSPI（SPI2_HOST）独立总线挂载 FATFS，并通过 VFS 暴露在 /sdcard。
// 挂载后所有文件操作走标准 C 库（fopen/fread/fwrite/stat/opendir）。
// sdkconfig 已开启 CONFIG_FATFS_LFN_HEAP 与 CONFIG_FATFS_API_ENCODING_UTF_8，
// 因此中文长文件名路径可直接使用。
//
// 引脚（eego_pins.h）：SCLK=39 MISO=40 MOSI=38 CS=47，20MHz

#include "sd_card.h"
#include "eego_pins.h"

#include <cstring>
#include <cerrno>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace eego {

static const char* TAG = "eego.sd";
constexpr const char* kMountPoint = "/sdcard";
constexpr size_t kMaxFiles = 5;
constexpr size_t kAllocUnit = 16 * 1024;
constexpr size_t kPathBuf = 512;

SdCard& SdCard::instance() {
    static SdCard inst;
    return inst;
}

// 将相对 /sdcard 的路径拼成完整 VFS 路径：
//   "books/a.epub"  -> "/sdcard/books/a.epub"
//   "/books/a.epub" -> "/sdcard/books/a.epub"
void SdCard::makeFullPath(const char* path, char* out, size_t outLen) const {
    if (path == nullptr || path[0] == '\0') {
        snprintf(out, outLen, "%s", kMountPoint);
    } else if (path[0] == '/') {
        snprintf(out, outLen, "%s%s", kMountPoint, path);
    } else {
        snprintf(out, outLen, "%s/%s", kMountPoint, path);
    }
}

// 递归创建目录（mkdir -p 语义，对已存在目录返回成功）
static bool makeDirsRecursive(const char* fullPath) {
    if (fullPath == nullptr || fullPath[0] != '/') return false;
    char tmp[kPathBuf];
    strncpy(tmp, fullPath, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';
    size_t len = strlen(tmp);
    if (len > 1 && tmp[len - 1] == '/') tmp[len - 1] = '\0';

    for (char* p = tmp + 1; *p; ++p) {
        if (*p == '/') {
            *p = '\0';
            if (::mkdir(tmp, 0775) != 0 && errno != EEXIST) {
                return false;
            }
            *p = '/';
        }
    }
    if (::mkdir(tmp, 0775) != 0 && errno != EEXIST) {
        return false;
    }
    return true;
}

esp_err_t SdCard::mount() {
    if (mounted_) {
        return ESP_OK;
    }

    // CS/MISO 使能内部上拉：卡未插紧/信号悬浮时提高成功率
    // （SCLK/MOSI 由 SPI 驱动主动驱动，不拉高，避免空闲时钟电平异常）
    gpio_set_pull_mode((gpio_num_t)SD_CS, GPIO_PULLUP_ONLY);
    gpio_set_pull_mode((gpio_num_t)SD_MISO, GPIO_PULLUP_ONLY);

    // 自动重试 + 降频回退：20MHz 失败后降到 10MHz（接触不良/老卡更稳）
    static const uint32_t kFreqKhz[3] = {20000, 10000, 10000};

    for (int attempt = 0; attempt < 3; attempt++) {
        if (attempt > 0) {
            ESP_LOGW(TAG, "SD mount retry %d/%d @ %u kHz",
                     attempt + 1, 3, (unsigned)kFreqKhz[attempt]);
            vTaskDelay(pdMS_TO_TICKS(300));
        }
        esp_err_t ret = mountOnce(kFreqKhz[attempt]);
        if (ret == ESP_OK) {
            return ESP_OK;
        }
        ESP_LOGW(TAG, "SD mount attempt %d/%d failed: %s",
                 attempt + 1, 3, esp_err_to_name(ret));
        // 失败后释放总线，下一次可重新初始化
        spi_bus_free(SPI2_HOST);
        card_ = nullptr;
    }

    ESP_LOGW(TAG, "SD card mount failed after retries "
                  "(cs=%d sclk=%d miso=%d mosi=%d) — 请检查卡是否插好或换一张卡",
             SD_CS, SD_SCLK, SD_MISO, SD_MOSI);
    return ESP_ERR_NOT_FOUND;
}

// 单次挂载尝试（指定 SPI 频率）
esp_err_t SdCard::mountOnce(uint32_t freqKhz) {
    // SDSPI 主机配置：SPI2_HOST（ESP32-S3 上的 HSPI）
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI2_HOST;
    host.max_freq_khz = freqKhz;

    // 独立 SPI 总线初始化（不与显示 EPD-SPI 共用）
    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = SD_MOSI;
    bus_cfg.miso_io_num = SD_MISO;
    bus_cfg.sclk_io_num = SD_SCLK;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = 4092;

    esp_err_t ret = spi_bus_initialize(SPI2_HOST, &bus_cfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }

    // SDSPI 设备配置（CS 引脚 + 绑定主机）
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_config.gpio_cs = static_cast<gpio_num_t>(SD_CS);
    slot_config.host_id = SPI2_HOST;

    // VFS FAT 挂载配置
    esp_vfs_fat_mount_config_t mount_config = {};
    mount_config.format_if_mount_failed = false;  // 不自动格式化，避免误删数据
    mount_config.max_files = kMaxFiles;
    mount_config.allocation_unit_size = kAllocUnit;

    ret = esp_vfs_fat_sdspi_mount(kMountPoint, &host, &slot_config, &mount_config, &card_);
    if (ret != ESP_OK) {
        card_ = nullptr;
        return ret;
    }

    mounted_ = true;
    ESP_LOGI(TAG, "SD card mounted at %s", kMountPoint);
    sdmmc_card_print_info(stdout, card_);
    return ESP_OK;
}

void SdCard::unmount() {
    if (!mounted_) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(kMountPoint, card_);
    card_ = nullptr;
    mounted_ = false;
    spi_bus_free(SPI2_HOST);
    ESP_LOGI(TAG, "SD card unmounted");
}

bool SdCard::exists(const char* path) {
    if (!mounted_ || path == nullptr) return false;
    char full[kPathBuf];
    makeFullPath(path, full, sizeof(full));
    struct stat st;
    return (::stat(full, &st) == 0);
}

bool SdCard::mkdir(const char* path) {
    if (!mounted_ || path == nullptr) return false;
    char full[kPathBuf];
    makeFullPath(path, full, sizeof(full));
    return makeDirsRecursive(full);
}

bool SdCard::remove(const char* path) {
    if (!mounted_ || path == nullptr) return false;
    char full[kPathBuf];
    makeFullPath(path, full, sizeof(full));
    return (::remove(full) == 0);
}

bool SdCard::rename(const char* from, const char* to) {
    if (!mounted_ || from == nullptr || to == nullptr) return false;
    char fullFrom[kPathBuf];
    char fullTo[kPathBuf];
    makeFullPath(from, fullFrom, sizeof(fullFrom));
    makeFullPath(to, fullTo, sizeof(fullTo));
    return (::rename(fullFrom, fullTo) == 0);
}

int SdCard::listDir(const char* path, DirEntry* entries, int maxEntries) {
    if (!mounted_ || path == nullptr || entries == nullptr || maxEntries <= 0) return 0;
    char full[kPathBuf];
    makeFullPath(path, full, sizeof(full));

    DIR* dir = ::opendir(full);
    if (dir == nullptr) {
        ESP_LOGW(TAG, "opendir failed: %s (%s)", full, strerror(errno));
        return 0;
    }

    int count = 0;
    struct dirent* ent;
    while (count < maxEntries && (ent = ::readdir(dir)) != nullptr) {
        // 跳过 "." / ".."
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) {
            continue;
        }
        DirEntry& e = entries[count];
        strncpy(e.name, ent->d_name, sizeof(e.name) - 1);
        e.name[sizeof(e.name) - 1] = '\0';

        // 通过 stat 取类型与大小（d_type 在 FATFS 下不可靠）
        char child[kPathBuf + 256];  // 预留 full(511) + '/' + d_name(255) + '\0'
        snprintf(child, sizeof(child), "%s/%s", full, ent->d_name);
        struct stat st;
        if (::stat(child, &st) == 0) {
            e.isDir = S_ISDIR(st.st_mode);
            e.size = static_cast<size_t>(st.st_size);
        } else {
            e.isDir = (ent->d_type == DT_DIR);
            e.size = 0;
        }
        count++;
    }
    ::closedir(dir);
    return count;
}

FILE* SdCard::open(const char* path, const char* mode) {
    if (!mounted_ || path == nullptr || mode == nullptr) return nullptr;
    char full[kPathBuf];
    makeFullPath(path, full, sizeof(full));
    return ::fopen(full, mode);
}

size_t SdCard::readFile(const char* path, uint8_t* buffer, size_t maxLen) {
    if (!mounted_ || path == nullptr || buffer == nullptr || maxLen == 0) return 0;
    FILE* f = open(path, "rb");
    if (f == nullptr) {
        return 0;
    }
    size_t total = ::fread(buffer, 1, maxLen, f);
    ::fclose(f);
    return total;
}

bool SdCard::writeFile(const char* path, const uint8_t* data, size_t len) {
    if (!mounted_ || path == nullptr || (data == nullptr && len > 0)) return false;
    FILE* f = open(path, "wb");
    if (f == nullptr) {
        return false;
    }
    size_t written = (len > 0) ? ::fwrite(data, 1, len, f) : 0;
    ::fclose(f);
    return (written == len);
}

size_t SdCard::fileSize(const char* path) {
    if (!mounted_ || path == nullptr) return 0;
    char full[kPathBuf];
    makeFullPath(path, full, sizeof(full));
    struct stat st;
    if (::stat(full, &st) != 0) {
        return 0;
    }
    return static_cast<size_t>(st.st_size);
}

}  // namespace eego
