// storage.cpp — SD 卡存储管理层实现（cJSON）
//
// 管理以下文件（路径相对于 /sdcard 根）：
//   /weread/config.json       — 配置（WiFi + 版式）
//   /weread/progress.json     — 阅读进度（{ bookId: {c, p, t}, ... }）
//   /weread/state.json        — 应用状态（上次阅读的书+章节，一次性恢复标记）
//   /weread/shelf_cache.json  — 书架缓存（[ {id, t, a, c, f, p}, ... ]）
//   /weread/imgoff.json       — 图片开关（{ bookId: true, ... }）
//   /weread/debug.log         — 滚动日志（64KB 上限，超了清零重写）
//   /weread/cache/<bookId>/   — 章节缓存目录
//   /weread/img2/<md5[:2]>/   — 图片缓存目录
//   /（根目录）               — 字体文件（.eefont/.bin，用户可自定义文件名）
#include "storage.h"
#include "sd_card.h"

#include <cstring>
#include <cstdio>
#include <cstdarg>
#include <string>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"

namespace eego {

static const char* TAG = "eego.stor";

#define SD_DEBUG_LOG_MAX  (64 * 1024)  // 日志上限 64KB

Storage& Storage::instance() {
    static Storage inst;
    return inst;
}

bool Storage::ensureDir(const char* path) {
    return SdCard::instance().mkdir(path);
}

// ============================ 辅助函数 ============================

// 读文件全部内容到 std::string
static bool readJsonFile(const char* path, std::string& out) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;
    FILE* f = sd.open(path, "rb");
    if (!f) return false;
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
    }
    fclose(f);
    return !out.empty();
}

// 安全拷贝字符串到固定长度缓冲区
static void safeCopy(const char* src, char* dst, size_t dstLen) {
    if (dstLen == 0) return;
    if (src) {
        strncpy(dst, src, dstLen - 1);
        dst[dstLen - 1] = '\0';
    } else {
        dst[0] = '\0';
    }
}

// cJSON 字段取字符串（NULL 安全）
static const char* jStr(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_GetStringValue(item);
}

// cJSON 字段取整数（带默认值）
static int jInt(cJSON* obj, const char* key, int def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!item || !cJSON_IsNumber(item)) return def;
    return item->valueint;
}

// 写 JSON 字符串到文件
static bool writeJsonStr(const char* path, const char* str) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;
    return sd.writeFile(path, (const uint8_t*)str, strlen(str));
}

// ============================ 目录初始化 ============================

bool Storage::initDirectories() {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) {
        ESP_LOGW(TAG, "SD not ready, skip initDirectories");
        return false;
    }
    bool ok = true;
    ok &= ensureDir(SD_WEREAD_DIR);
    ok &= ensureDir(SD_CACHE_DIR);
    ok &= ensureDir(SD_IMG2_DIR);
    // 字体放根目录，无需创建 /font
    ESP_LOGI(TAG, "directories initialized: %s", ok ? "ok" : "partial");
    return ok;
}

// ============================ 配置文件 ============================

bool Storage::loadConfig(AppConfig& config) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;

    std::string buf;
    if (!readJsonFile(SD_CONFIG_PATH, buf)) {
        ESP_LOGI(TAG, "config.json not found");
        return false;
    }

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) {
        ESP_LOGW(TAG, "config.json parse failed");
        return false;
    }

    config = {};
    // wifi_list 数组
    config.wifiCount = 0;
    cJSON* arr = cJSON_GetObjectItem(root, "wifi_list");
    if (cJSON_IsArray(arr)) {
        cJSON* ap = nullptr;
        cJSON_ArrayForEach(ap, arr) {
            if (config.wifiCount >= 5) break;
            const char* ssid = jStr(ap, "ssid");
            if (ssid && ssid[0]) {
                WifiAccount& w = config.wifiList[config.wifiCount++];
                safeCopy(ssid, w.ssid, sizeof(w.ssid));
                safeCopy(jStr(ap, "pass"), w.password, sizeof(w.password));
            }
        }
    }
    // 兼容老格式单账号
    const char* legacySsid = jStr(root, "wifi_ssid");
    if (legacySsid && legacySsid[0]) {
        bool found = false;
        for (int i = 0; i < config.wifiCount; i++) {
            if (strcmp(config.wifiList[i].ssid, legacySsid) == 0) { found = true; break; }
        }
        if (!found && config.wifiCount < 5) {
            WifiAccount& w = config.wifiList[config.wifiCount++];
            safeCopy(legacySsid, w.ssid, sizeof(w.ssid));
            safeCopy(jStr(root, "wifi_pass"), w.password, sizeof(w.password));
        }
    }

    safeCopy(jStr(root, "font"), config.fontPath, sizeof(config.fontPath));
    config.lineHeight       = jInt(root, "line_height", 100);
    config.paragraphSpacing = jInt(root, "para_gap", 1);
    config.firstLineIndent  = jInt(root, "para_indent", 2);
    // show_images 缺失时默认开图
    {
        cJSON* si = cJSON_GetObjectItem(root, "show_images");
        config.showImages = si ? cJSON_IsTrue(si) : true;
    }
    config.fontSize = jInt(root, "font_size", 26);

    cJSON_Delete(root);
    return true;
}

bool Storage::saveConfig(const AppConfig& config) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;

    // 读已有配置保留未知字段
    cJSON* root = nullptr;
    if (sd.exists(SD_CONFIG_PATH)) {
        std::string buf;
        if (readJsonFile(SD_CONFIG_PATH, buf)) {
            root = cJSON_Parse(buf.c_str());
        }
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) return false;

    // wifi_list
    cJSON* wifiArr = cJSON_CreateArray();
    for (int i = 0; i < config.wifiCount && i < 5; i++) {
        cJSON* ap = cJSON_CreateObject();
        cJSON_AddStringToObject(ap, "ssid", config.wifiList[i].ssid);
        cJSON_AddStringToObject(ap, "pass", config.wifiList[i].password);
        cJSON_AddItemToArray(wifiArr, ap);
    }
    cJSON_DeleteItemFromObject(root, "wifi_list");
    cJSON_AddItemToObject(root, "wifi_list", wifiArr);

    if (config.wifiCount > 0) {
        cJSON_DeleteItemFromObject(root, "wifi_ssid");
        cJSON_AddStringToObject(root, "wifi_ssid", config.wifiList[0].ssid);
        cJSON_DeleteItemFromObject(root, "wifi_pass");
        cJSON_AddStringToObject(root, "wifi_pass", config.wifiList[0].password);
    }

    cJSON_DeleteItemFromObject(root, "font");
    cJSON_AddStringToObject(root, "font", config.fontPath);
    cJSON_DeleteItemFromObject(root, "line_height");
    cJSON_AddNumberToObject(root, "line_height", config.lineHeight);
    cJSON_DeleteItemFromObject(root, "para_gap");
    cJSON_AddNumberToObject(root, "para_gap", config.paragraphSpacing);
    cJSON_DeleteItemFromObject(root, "para_indent");
    cJSON_AddNumberToObject(root, "para_indent", config.firstLineIndent);
    cJSON_DeleteItemFromObject(root, "show_images");
    cJSON_AddBoolToObject(root, "show_images", config.showImages);
    cJSON_DeleteItemFromObject(root, "font_size");
    cJSON_AddNumberToObject(root, "font_size", config.fontSize);

    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return false;

    ensureDir(SD_WEREAD_DIR);
    bool ok = writeJsonStr(SD_CONFIG_PATH, str);
    free(str);
    return ok;
}

// ============================ 阅读状态（state.json） ============================

bool Storage::loadState(ReadingState& out) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;

    std::string buf;
    if (!readJsonFile(SD_STATE_PATH, buf)) return false;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) return false;

    out = {};
    safeCopy(jStr(root, "bookId"), out.bookId, sizeof(out.bookId));
    out.chapterIdx = jInt(root, "c", 0);
    out.page       = jInt(root, "p", 0);
    safeCopy(jStr(root, "title"),  out.title,  sizeof(out.title));
    safeCopy(jStr(root, "cover"),  out.cover,  sizeof(out.cover));
    safeCopy(jStr(root, "format"), out.format, sizeof(out.format));

    cJSON_Delete(root);
    return out.bookId[0] != '\0';
}

bool Storage::saveState(const ReadingState& in) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;

    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    cJSON_AddStringToObject(root, "bookId", in.bookId);
    cJSON_AddNumberToObject(root, "c", in.chapterIdx);
    cJSON_AddNumberToObject(root, "p", in.page);
    cJSON_AddStringToObject(root, "title",  in.title);
    cJSON_AddStringToObject(root, "cover",  in.cover);
    cJSON_AddStringToObject(root, "format", in.format);

    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return false;

    ensureDir(SD_WEREAD_DIR);
    bool ok = writeJsonStr(SD_STATE_PATH, str);
    free(str);
    return ok;
}

bool Storage::removeState() {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return false;
    if (!sd.exists(SD_STATE_PATH)) return true;
    return sd.remove(SD_STATE_PATH);
}

// ============================ 书架缓存（shelf_cache.json） ============================

int Storage::loadShelfCache(ShelfEntry* entries, int maxCount) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !entries || maxCount <= 0) return 0;

    std::string buf;
    if (!readJsonFile(SD_SHELF_PATH, buf)) return 0;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root || !cJSON_IsArray(root)) {
        if (root) cJSON_Delete(root);
        return 0;
    }

    int count = 0;
    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, root) {
        if (count >= maxCount) break;
        ShelfEntry& e = entries[count++];
        safeCopy(jStr(item, "id"), e.bookId, sizeof(e.bookId));
        safeCopy(jStr(item, "t"),  e.title,  sizeof(e.title));
        safeCopy(jStr(item, "a"),  e.author, sizeof(e.author));
        safeCopy(jStr(item, "c"),  e.cover,  sizeof(e.cover));
        safeCopy(jStr(item, "f"),  e.format, sizeof(e.format));
        e.progress = jInt(item, "p", 0);
    }
    cJSON_Delete(root);
    return count;
}

bool Storage::saveShelfCache(const ShelfEntry* entries, int count) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !entries || count < 0) return false;

    cJSON* root = cJSON_CreateArray();
    if (!root) return false;
    for (int i = 0; i < count; i++) {
        const ShelfEntry& e = entries[i];
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", e.bookId);
        cJSON_AddStringToObject(o, "t",  e.title);
        cJSON_AddStringToObject(o, "a",  e.author);
        cJSON_AddStringToObject(o, "c",  e.cover);
        cJSON_AddStringToObject(o, "f",  e.format);
        cJSON_AddNumberToObject(o, "p",  e.progress);
        cJSON_AddItemToArray(root, o);
    }

    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return false;

    ensureDir(SD_WEREAD_DIR);
    bool ok = writeJsonStr(SD_SHELF_PATH, str);
    free(str);
    return ok;
}

// ============================ 阅读进度（progress.json） ============================

bool Storage::loadProgress(const char* bookId, BookProgress& out) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return false;

    out = {};
    std::string buf;
    if (!readJsonFile(SD_PROGRESS_PATH, buf)) return false;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) return false;

    cJSON* entry = cJSON_GetObjectItem(root, bookId);
    if (entry) {
        out.chapterIdx = jInt(entry, "c", 0);
        out.page       = jInt(entry, "p", 0);
        out.percent    = jInt(entry, "n", -1);
        safeCopy(jStr(entry, "t"), out.title, sizeof(out.title));
    }
    cJSON_Delete(root);
    return entry != nullptr;
}

bool Storage::saveProgress(const char* bookId, const BookProgress& in) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return false;

    // 读已有进度（保留其它书的记录）
    cJSON* root = nullptr;
    if (sd.exists(SD_PROGRESS_PATH)) {
        std::string buf;
        if (readJsonFile(SD_PROGRESS_PATH, buf)) {
            root = cJSON_Parse(buf.c_str());
        }
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) return false;

    // 更新指定 bookId 的条目
    cJSON_DeleteItemFromObject(root, bookId);
    cJSON* entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "c", in.chapterIdx);
    cJSON_AddNumberToObject(entry, "p", in.page);
    cJSON_AddNumberToObject(entry, "n", in.percent);
    cJSON_AddStringToObject(entry, "t", in.title);
    cJSON_AddItemToObject(root, bookId, entry);

    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return false;

    ensureDir(SD_WEREAD_DIR);
    bool ok = writeJsonStr(SD_PROGRESS_PATH, str);
    free(str);
    return ok;
}

// ============================ 图片开关（imgoff.json） ============================

bool Storage::loadImgOff(const char* bookId) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return false;

    std::string buf;
    if (!readJsonFile(SD_IMGOFF_PATH, buf)) return false;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) return false;

    cJSON* item = cJSON_GetObjectItem(root, bookId);
    bool off = cJSON_IsTrue(item);
    cJSON_Delete(root);
    return off;
}

bool Storage::saveImgOff(const char* bookId, bool off) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return false;

    // 读已有配置（保留其它书的开关）
    cJSON* root = nullptr;
    if (sd.exists(SD_IMGOFF_PATH)) {
        std::string buf;
        if (readJsonFile(SD_IMGOFF_PATH, buf)) {
            root = cJSON_Parse(buf.c_str());
        }
    }
    if (!root) root = cJSON_CreateObject();
    if (!root) return false;

    cJSON_DeleteItemFromObject(root, bookId);
    if (off) {
        cJSON_AddBoolToObject(root, bookId, true);
    }
    // off=false 时删除条目（不存 = 默认开图）

    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return false;

    ensureDir(SD_WEREAD_DIR);
    bool ok = writeJsonStr(SD_IMGOFF_PATH, str);
    free(str);
    return ok;
}

// ============================ 章节缓存（.blk 格式） ============================

// 每本书一个目录：/weread/cache/<bookId>/，互不错串，可整本清除
static std::string bookCacheDir(const std::string& bookId) {
    return std::string(SD_CACHE_DIR) + "/" + bookId;
}

static std::string chapterCachePath(const std::string& bookId, const std::string& chapterUid) {
    return bookCacheDir(bookId) + "/ch_" + chapterUid + ".blk";
}

bool Storage::chapterCacheSave(const char* bookId, const char* chapterUid,
                               const std::vector<weread_api::ContentBlock>& blocks) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId || !chapterUid || blocks.empty()) return false;

    ensureDir(SD_WEREAD_DIR);
    ensureDir(SD_CACHE_DIR);
    ensureDir(bookCacheDir(bookId).c_str());

    FILE* f = sd.open(chapterCachePath(bookId, chapterUid).c_str(), "wb");
    if (!f) return false;
    fprintf(f, "B7\n");
    for (const auto& b : blocks) {
        if (b.is_image) {
            fprintf(f, "I %s\n", b.text.c_str());
        } else {
            fprintf(f, "T %d %d\n", (int)b.style, (int)b.text.length());
            fwrite(b.text.data(), 1, b.text.length(), f);
            fprintf(f, "\n");
        }
    }
    fclose(f);
    return true;
}

bool Storage::chapterCacheLoad(const char* bookId, const char* chapterUid,
                               std::vector<weread_api::ContentBlock>& out) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId || !chapterUid) return false;

    FILE* f = sd.open(chapterCachePath(bookId, chapterUid).c_str(), "rb");
    if (!f) return false;

    char magic[16] = {0};
    if (fgets(magic, sizeof(magic), f) == nullptr) { fclose(f); return false; }
    // 注意 println 写的是 \r\n：每行都要 trim 掉 \r，否则魔数/URL 全坏
    size_t len = strlen(magic);
    while (len > 0 && (magic[len - 1] == '\n' || magic[len - 1] == '\r')) magic[--len] = '\0';
    if (strcmp(magic, "B7") != 0) { fclose(f); return false; }  // 更早版本可能含脏数据，作废

    out.clear();
    char head[512];
    while (fgets(head, sizeof(head), f) != nullptr) {
        len = strlen(head);
        while (len > 0 && (head[len - 1] == '\n' || head[len - 1] == '\r')) head[--len] = '\0';
        if (head[0] == 'I' && head[1] == ' ') {
            weread_api::ContentBlock b;
            b.is_image = true;
            b.text = head + 2;
            out.push_back(b);
        } else if (head[0] == 'T' && head[1] == ' ') {
            int style = 0, blen = 0;
            if (sscanf(head + 2, "%d %d", &style, &blen) != 2 || blen <= 0 || blen > 200000) break;
            weread_api::ContentBlock b;
            b.is_image = false;
            b.style = (uint8_t)style;
            b.text.resize(blen);
            size_t got = fread(&b.text[0], 1, blen, f);
            if (got != (size_t)blen) break;
            out.push_back(b);
            // 跳过块尾 \r\n
            int c;
            while ((c = fgetc(f)) == '\r' || c == '\n') {}
            if (c != EOF) ungetc(c, f);
            else break;
        } else if (head[0] == '\0') {
            continue;
        } else {
            break;
        }
    }
    fclose(f);
    return !out.empty();
}

bool Storage::chapterCacheExists(const char* bookId, const char* chapterUid) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId || !chapterUid) return false;
    return sd.exists(chapterCachePath(bookId, chapterUid).c_str());
}

// ============================ 目录缓存（toc.json） ============================

bool Storage::saveTocCache(const char* bookId, const std::vector<weread_api::ChapterEntry>& chapters,
                           const char* format) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId || chapters.empty()) return false;

    ensureDir(SD_WEREAD_DIR);
    ensureDir(SD_CACHE_DIR);
    ensureDir(bookCacheDir(bookId).c_str());

    cJSON* root = cJSON_CreateObject();
    if (!root) return false;
    if (format) cJSON_AddStringToObject(root, "f", format);
    cJSON* arr = cJSON_AddArrayToObject(root, "ch");
    for (const auto& ch : chapters) {
        cJSON* o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "u", ch.chapterUid.c_str());
        cJSON_AddStringToObject(o, "t", ch.title.c_str());
        cJSON_AddNumberToObject(o, "w", ch.wordCount);
        cJSON_AddBoolToObject(o, "p", ch.paid);
        cJSON_AddItemToArray(arr, o);
    }
    char* str = cJSON_Print(root);
    cJSON_Delete(root);
    if (!str) return false;

    std::string path = bookCacheDir(bookId) + "/toc.json";
    bool ok = writeJsonStr(path.c_str(), str);
    free(str);
    return ok;
}

bool Storage::loadTocCache(const char* bookId, std::vector<weread_api::ChapterEntry>& out,
                           std::string& format) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return false;

    std::string buf;
    std::string path = bookCacheDir(bookId) + "/toc.json";
    if (!readJsonFile(path.c_str(), buf)) return false;

    cJSON* root = cJSON_Parse(buf.c_str());
    if (!root) return false;

    out.clear();
    format.clear();
    const char* f = jStr(root, "f");
    if (f) format = f;

    cJSON* arr = cJSON_GetObjectItem(root, "ch");
    if (cJSON_IsArray(arr)) {
        cJSON* o = nullptr;
        cJSON_ArrayForEach(o, arr) {
            const char* u = jStr(o, "u");
            const char* t = jStr(o, "t");
            if (!u || !u[0]) continue;
            weread_api::ChapterEntry ch;
            ch.chapterUid = u;
            ch.title = t ? t : "";
            ch.wordCount = jInt(o, "w", 0);
            ch.paid = cJSON_IsTrue(cJSON_GetObjectItem(o, "p"));
            ch.chapterIdx = (int)out.size();
            out.push_back(ch);
        }
    }
    cJSON_Delete(root);
    return !out.empty();
}

bool Storage::bookDownloadDone(const char* bookId, int total) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId || total <= 0) return false;
    std::string path = bookCacheDir(bookId) + "/done.flag";
    if (!sd.exists(path.c_str())) return false;
    uint8_t buf[16] = {0};
    size_t n = sd.readFile(path.c_str(), buf, sizeof(buf) - 1);
    if (n == 0) return false;
    buf[n] = '\0';
    return atoi((const char*)buf) == total;
}

void Storage::bookDownloadDoneMark(const char* bookId, int total) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return;
    ensureDir(SD_WEREAD_DIR);
    ensureDir(SD_CACHE_DIR);
    ensureDir(bookCacheDir(bookId).c_str());
    char buf[16];
    snprintf(buf, sizeof(buf), "%d\n", total);
    sd.writeFile((bookCacheDir(bookId) + "/done.flag").c_str(), (const uint8_t*)buf, strlen(buf));
}

bool Storage::deleteBookCache(const char* bookId) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready() || !bookId) return false;
    std::string dir = bookCacheDir(bookId);
    if (!sd.exists(dir.c_str())) return true;  // 不存在视为已删除

    // 分批列出并删除，避免在栈上放大数组（DirEntry 含 256 字节 name）导致栈溢出。
    // 每批删完再列，直到删空。
    const int kBatch = 8;
    SdCard::DirEntry entries[kBatch];
    for (;;) {
        int n = sd.listDir(dir.c_str(), entries, kBatch);
        if (n <= 0) break;
        for (int i = 0; i < n; i++) {
            std::string p = dir + "/" + entries[i].name;
            if (entries[i].isDir) {
                // 理论无子目录，但保险起见递归删除
                SdCard::DirEntry sub[4];
                int sn = sd.listDir(p.c_str(), sub, 4);
                for (int j = 0; j < sn; j++) sd.remove((p + "/" + sub[j].name).c_str());
                sd.remove(p.c_str());
            } else {
                sd.remove(p.c_str());
            }
        }
    }
    sd.remove(dir.c_str());
    ESP_LOGI(TAG, "已删除整本下载缓存: %s", bookId);
    return true;
}

// ============================ debug.log 滚动日志 ============================

void Storage::log(const char* fmt, ...) {
    SdCard& sd = SdCard::instance();
    if (!sd.ready()) return;

    ensureDir(SD_WEREAD_DIR);

    // 超过 64KB 则清零重写（防无限膨胀）
    size_t sz = sd.fileSize(SD_LOG_PATH);
    const char* mode = (sz > SD_DEBUG_LOG_MAX) ? "wb" : "ab";

    FILE* f = sd.open(SD_LOG_PATH, mode);
    if (!f) return;

    // 毫秒时间戳（开机起，与 millis() 语义一致）
    uint32_t ms = (uint32_t)(esp_timer_get_time() / 1000);

    // 格式化消息体
    char buf[256];
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    if (n < 0) { fclose(f); return; }

    fprintf(f, "[%lu] %s\n", (unsigned long)ms, buf);
    fclose(f);
}

}  // namespace eego
