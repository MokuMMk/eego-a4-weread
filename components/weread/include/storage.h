// storage.h — SD 卡存储管理层
// 封装 /sdcard/weread/ 下的所有持久化操作：配置、阅读状态、进度、
// 书架缓存、图片开关、滚动日志。文件路径相对于 /sdcard 根。
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#include "config.h"  // AppConfig
#include "weread_api.h"  // weread_api::ContentBlock

#include <string>
#include <vector>

namespace eego {

// ---------------- 路径常量（相对于 /sdcard） ----------------
#define SD_WEREAD_DIR    "/weread"
#define SD_CACHE_DIR     "/weread/cache"
#define SD_IMG2_DIR      "/weread/img2"
// 字体文件放在 SD 卡根目录（用户可自定义文件名，应用自动发现）
#define SD_CONFIG_PATH   "/weread/config.json"
#define SD_PROGRESS_PATH "/weread/progress.json"
#define SD_STATE_PATH    "/weread/state.json"
#define SD_SHELF_PATH    "/weread/shelf_cache.json"
#define SD_IMGOFF_PATH   "/weread/imgoff.json"
#define SD_LOG_PATH      "/weread/debug.log"

// ---------------- 数据结构 ----------------

// 阅读状态（state.json）：开机/休眠恢复时直接回到上次阅读页
struct ReadingState {
    char bookId[32];     // 当前书 ID
    int  chapterIdx;     // 章节下标
    int  page;           // 页码
    char title[128];     // 书名（快速恢复用，不依赖书架加载）
    char cover[256];     // 封面 URL
    char format[16];     // 格式（epub/...）
};

// 单本书阅读进度（progress.json 中按 bookId 索引的条目）
struct BookProgress {
    int  chapterIdx;     // "c" 章节下标
    int  page;           // "p" 页码
    char title[128];     // "t" 书名（离线书架兜底用）
    int  percent;        // "n" 真实阅读进度百分比（-1=未知/未计算，用服务器进度兜底）
};

// 书架条目（shelf_cache.json 数组元素）
struct ShelfEntry {
    char bookId[32];     // "id"
    char title[128];     // "t"
    char author[64];     // "a"
    char cover[256];     // "c"
    char format[16];     // "f"
    int  progress;       // "p" 阅读进度百分比
};

// ---------------- Storage 单例 ----------------
class Storage {
public:
    static Storage& instance();

    // 初始化 SD 卡目录结构（/weread、/weread/cache、/weread/img2）
    bool initDirectories();

    // ---- 配置文件 ----
    bool loadConfig(AppConfig& config);
    bool saveConfig(const AppConfig& config);

    // ---- 阅读状态（state.json，一次性恢复标记） ----
    bool loadState(ReadingState& out);
    bool saveState(const ReadingState& in);
    bool removeState();  // 显式退出阅读时清除恢复标记

    // ---- 书架缓存（shelf_cache.json） ----
    // 返回加载的条目数（0 = 失败或空）
    int  loadShelfCache(ShelfEntry* entries, int maxCount);
    bool saveShelfCache(const ShelfEntry* entries, int count);

    // ---- 阅读进度（progress.json，按 bookId 索引） ----
    bool loadProgress(const char* bookId, BookProgress& out);
    bool saveProgress(const char* bookId, const BookProgress& in);

    // ---- 图片开关（imgoff.json，按 bookId 索引） ----
    // 返回 true 表示该书插图已关闭
    bool loadImgOff(const char* bookId);
    bool saveImgOff(const char* bookId, bool off);

    // ---- 章节缓存（/weread/cache/<bookId>/ch_<uid>.blk，参考原版 .blk 格式） ----
    // 格式：首行魔数 "B7"，之后每块 "I <url>\n"（图片）或 "T <style> <len>\n<原始字节>\n"（文本）
    bool chapterCacheSave(const char* bookId, const char* chapterUid,
                          const std::vector<weread_api::ContentBlock>& blocks);
    bool chapterCacheLoad(const char* bookId, const char* chapterUid,
                          std::vector<weread_api::ContentBlock>& out);
    bool chapterCacheExists(const char* bookId, const char* chapterUid);

    // ---- 目录缓存（/weread/cache/<bookId>/toc.json，供离线打开已下载的书） ----
    bool saveTocCache(const char* bookId, const std::vector<weread_api::ChapterEntry>& chapters,
                      const char* format);
    bool loadTocCache(const char* bookId, std::vector<weread_api::ChapterEntry>& out,
                      std::string& format);

    // ---- 整本下载完成标记（/weread/cache/<bookId>/done.flag 存章节总数） ----
    // 与当前章节数一致才算"已下载"，目录变多后自动失效
    bool bookDownloadDone(const char* bookId, int total);
    void bookDownloadDoneMark(const char* bookId, int total);

    // 删除某本书的整本下载缓存（/weread/cache/<bookId>/ 整个目录，含 done.flag）
    bool deleteBookCache(const char* bookId);

    // ---- debug.log（64KB 滚动日志） ----
    // 追加一行带毫秒时间戳的日志；超 64KB 自动清零重写
    void log(const char* fmt, ...);

private:
    Storage() = default;
    bool ensureDir(const char* path);  // mkdir -p
};

}  // namespace eego
