// weread_api.cpp — 实现
// 纯 ESP-IDF：std::string 替代 Arduino String，cJSON 替代 ArduinoJson，
// mbedtls 提供 SHA256，esp_http_client 用于正文分片会话。
// EPUB 整本 zip 解包用 miniz 库实现（少数公版/原版书才走该路径）。
#include "weread_api.h"
#include "weread_client.h"
#include "weread_crypto.h"
#include "storage.h"

#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "cJSON.h"
#include "mbedtls/sha256.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cctype>

// miniz C 库：EPUB 整本 zip 解包
// 注意：ESP-IDF esp_rom 自带 miniz.h 并 #define MINIZ_NO_ARCHIVE_APIS（禁用 zip API），
// 其 include 路径优先级高于本组件。因此将我们的头文件重命名为 miniz_full.h 以避免冲突。
#include "miniz_full.h"

// 书架流式扫描上限：2400+ 本全量 cJSON 建树会撑爆 PSRAM，
// 流式扫描只保留前 SHELF_MAX_BOOKS 本（与 initShelf 缓存容量匹配）。
#define SHELF_MAX_BOOKS 200

namespace weread_api {

using eego::WR;
using eego::WEREAD_USER_AGENT;
using eego::HttpResponse;
// WEREAD_HOST_WEB / WEREAD_HOST_I / HTTP_TIMEOUT_MS 是宏，自动可见，无需 using

static const char* TAG = "eego.api";

// ---- 辅助 ----

static int64_t millis_now() { return esp_timer_get_time() / 1000; }

static void debug_log(const std::string& s) {
    eego::Storage::instance().log("%s", s.c_str());
}

// 在 [data,len) 内从 from 起找子串，返回下标或 -1（限长 memcmp 搜索，不要求 \0 结尾）
static int find_sub(const char* data, size_t len, size_t from, const char* needle, size_t nlen) {
    if (!nlen || from + nlen > len) return -1;
    for (size_t i = from; i + nlen <= len; i++)
        if (memcmp(data + i, needle, nlen) == 0) return (int)i;
    return -1;
}

static int find_sub(const char* data, size_t len, const char* needle, size_t nlen) {
    return find_sub(data, len, 0, needle, nlen);
}

// UTF-8 安全切点：p[end] 若落在多字节字符中间（10xxxxxx），回退到字符边界
static size_t utf8_safe_cut(const char* p, size_t end) {
    while (end > 0 && (p[end] & 0xC0) == 0x80) end--;
    return end;
}

static std::string trim(std::string s) {
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return "";
    size_t end = s.find_last_not_of(" \t\n\r");
    return s.substr(start, end - start + 1);
}

// cJSON 取值辅助
static const char* cj_str(cJSON* obj, const char* key) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    return cJSON_GetStringValue(item);
}
static std::string cj_strval(cJSON* obj, const char* key) {
    const char* s = cj_str(obj, key);
    return s ? std::string(s) : std::string();
}
static int cj_int(cJSON* obj, const char* key, int def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!item || !cJSON_IsNumber(item)) return def;
    return item->valueint;
}
static bool cj_bool(cJSON* obj, const char* key, bool def) {
    cJSON* item = cJSON_GetObjectItem(obj, key);
    if (!item) return def;
    if (cJSON_IsTrue(item)) return true;
    if (cJSON_IsFalse(item)) return false;
    return def;
}
// cJSON 值转字符串（可能是数字）
static std::string cj_to_str(cJSON* item) {
    if (!item) return "";
    if (cJSON_IsString(item)) return item->valuestring ? std::string(item->valuestring) : std::string();
    if (cJSON_IsNumber(item)) return std::to_string(item->valueint);
    return "";
}
// cJSON 按 key 取字符串值（兼容数字类型，如 chapterUid 可能是 int）
static std::string cj_strval_n(cJSON* obj, const char* key) {
    return cj_to_str(cJSON_GetObjectItem(obj, key));
}

// 完整 JSON 必以 } 收尾
static bool json_tail_complete(const HttpResponse& r) {
    size_t n = r.length();
    while (n > 0) {
        char c = r.data()[n - 1];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') { n--; continue; }
        return c == '}';
    }
    return false;
}

static void shelf_debug_dump(const HttpResponse& r, const std::string& err) {
    size_t len = r.length();
    std::string s = "shelf 失败 err=" + err + " len=" + std::to_string(len) + " head=";
    s.append(r.data(), len < 120 ? len : 120);
    if (len > 100) { s += " tail="; s.append(r.data() + len - 100, 100); }
    debug_log(s);
}

// 检查 -2012（服务器两种字段名都有：errcode / errCode）
static int check_errcode(cJSON* root) {
    cJSON* ec = cJSON_GetObjectItem(root, "errcode");
    if (!ec || cJSON_IsNull(ec)) ec = cJSON_GetObjectItem(root, "errCode");
    if (ec && cJSON_IsNumber(ec)) return ec->valueint;
    return 0;
}

// ---- 书架 ----
//
// 流式扫描相关辅助：在 PSRAM 大缓冲上就地解析，避免 cJSON 对 2400+ 本书
// 全量建树导致内存溢出。扫描器只识别 books[] / bookProgress[] 顶层字段，
// 不构造完整的 cJSON 树。

// 在 [data,len) 中查找形如 "key":[ 的数组键，返回 [ 的位置，或 -1。
// key_quoted 形如 "\"books\""。会跳过 "key" 之后非数组的同名键。
static int find_array_key(const char* data, size_t len, const char* key_quoted) {
    size_t klen = strlen(key_quoted);
    size_t from = 0;
    for (;;) {
        int found = find_sub(data, len, from, key_quoted, klen);
        if (found < 0) return -1;
        size_t p = (size_t)found + klen;
        while (p < len && (data[p]==' '||data[p]=='\t'||data[p]=='\n'||data[p]=='\r')) p++;
        if (p < len && data[p] == ':') {
            p++;
            while (p < len && (data[p]==' '||data[p]=='\t'||data[p]=='\n'||data[p]=='\r')) p++;
            if (p < len && data[p] == '[') return (int)p;
        }
        from = (size_t)found + klen;
    }
}

// 从 obj_start（指向 '{'）查找匹配的 '}'，返回其位置；正确处理字符串内的括号与转义。
static size_t find_matching_brace(const char* data, size_t len, size_t obj_start) {
    if (obj_start >= len || data[obj_start] != '{') return (size_t)-1;
    int depth = 0;
    bool in_str = false, esc = false;
    for (size_t i = obj_start; i < len; i++) {
        char c = data[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            continue;
        }
        if (c == '"') { in_str = true; continue; }
        if (c == '{') depth++;
        else if (c == '}') { depth--; if (depth == 0) return i; }
    }
    return (size_t)-1;
}

// 从 val_pos（应指向开头的 "）读取 JSON 字符串值，返回解码后内容。
// 处理 \" \\ \/ \n \t \r \b \f 及 \uXXXX（转 UTF-8）。
static std::string extract_str_value(const char* data, size_t len, size_t val_pos) {
    std::string out;
    if (val_pos >= len || data[val_pos] != '"') return out;
    size_t i = val_pos + 1;
    while (i < len) {
        char c = data[i];
        if (c == '\\' && i + 1 < len) {
            char n = data[i + 1];
            switch (n) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'n': out += '\n'; break;
                case 't': out += '\t'; break;
                case 'r': out += '\r'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'u': {
                    if (i + 6 <= len) {
                        char hex[5] = { data[i+2], data[i+3], data[i+4], data[i+5], 0 };
                        unsigned cp = (unsigned)strtoul(hex, nullptr, 16);
                        if (cp < 0x80) out += (char)cp;
                        else if (cp < 0x800) {
                            out += (char)(0xC0 | (cp >> 6));
                            out += (char)(0x80 | (cp & 0x3F));
                        } else {
                            out += (char)(0xE0 | (cp >> 12));
                            out += (char)(0x80 | ((cp >> 6) & 0x3F));
                            out += (char)(0x80 | (cp & 0x3F));
                        }
                        i += 6;
                        continue;
                    }
                    out += n;
                    break;
                }
                default: out += n; break;
            }
            i += 2;
            continue;
        }
        if (c == '"') break;
        out += c;
        i++;
    }
    return out;
}

// 从 val_pos 读取 JSON 数字（支持负号、小数、指数），成功返回 true 并写出 double。
static bool extract_num_value(const char* data, size_t len, size_t val_pos, double* out_num) {
    if (val_pos >= len) return false;
    size_t i = val_pos;
    bool neg = false;
    if (data[i] == '-') { neg = true; i++; }
    if (i >= len || data[i] < '0' || data[i] > '9') return false;
    double v = 0;
    while (i < len && data[i] >= '0' && data[i] <= '9') { v = v * 10 + (data[i] - '0'); i++; }
    if (i < len && data[i] == '.') {
        i++;
        double frac = 0, div = 1;
        while (i < len && data[i] >= '0' && data[i] <= '9') { frac = frac * 10 + (data[i] - '0'); div *= 10; i++; }
        v += frac / div;
    }
    if (i < len && (data[i] == 'e' || data[i] == 'E')) {
        i++;
        bool eneg = false;
        if (i < len && data[i] == '-') { eneg = true; i++; }
        else if (i < len && data[i] == '+') i++;
        double e = 0;
        while (i < len && data[i] >= '0' && data[i] <= '9') { e = e * 10 + (data[i] - '0'); i++; }
        double m = 1;
        for (int k = 0; k < (int)e; k++) m *= 10;
        v *= eneg ? (1.0 / m) : m;
    }
    if (neg) v = -v;
    if (out_num) *out_num = v;
    return true;
}

// 扫描对象 [obj_start, obj_end] 范围内的顶层字段（depth==1），
// 对每个 "key": value 调用 cb(key_ptr, key_len, val_pos)，val_pos 指向 value 首字符。
// 正确跳过字符串值与嵌套对象/数组，避免误把值里的内容当成键。
template <typename Cb>
static void scan_object_fields(const char* data, size_t len,
                               size_t obj_start, size_t obj_end, Cb cb) {
    size_t i = obj_start + 1;
    int depth = 1;
    bool in_str = false, esc = false;
    while (i < obj_end && depth > 0) {
        char c = data[i];
        if (in_str) {
            if (esc) esc = false;
            else if (c == '\\') esc = true;
            else if (c == '"') in_str = false;
            i++;
            continue;
        }
        if (c == '"') {
            // 读取键名（到下一个未转义的 "）
            size_t key_start = i + 1;
            size_t j = key_start;
            while (j < obj_end) {
                if (data[j] == '\\') { j += 2; continue; }
                if (data[j] == '"') break;
                j++;
            }
            size_t key_len = j - key_start;
            i = (j < obj_end) ? j + 1 : j;  // 跳过闭合 "
            while (i < obj_end && (data[i]==' '||data[i]=='\t'||data[i]=='\n'||data[i]=='\r')) i++;
            if (i < obj_end && data[i] == ':') {
                i++;
                while (i < obj_end && (data[i]==' '||data[i]=='\t'||data[i]=='\n'||data[i]=='\r')) i++;
                size_t val_pos = i;
                if (depth == 1) cb(data + key_start, key_len, val_pos);
                // 若值为字符串，进入字符串状态以正确跳过其内容
                if (i < obj_end && data[i] == '"') { in_str = true; i++; }
            }
            continue;
        }
        if (c == '{' || c == '[') { depth++; i++; continue; }
        if (c == '}' || c == ']') { depth--; i++; continue; }
        i++;
    }
}

// 流式扫描书架响应：在原始缓冲上就地解析 books[] 与 bookProgress[]。
// 成功返回 true；找不到 books 或 bookProgress 数组、或解析出 0 本时返回 false
// （调用方回退到 cJSON）。
static bool parseBookshelfStream(const char* data, size_t len, std::vector<BookEntry>& out) {
    out.clear();
    if (!data || len < 16) return false;

    // 1. 定位 "books":[
    int books_arr = find_array_key(data, len, "\"books\"");
    if (books_arr < 0) return false;
    size_t p = (size_t)books_arr + 1;  // 跳过 [

    out.reserve(64);

    // 2. 遍历 books[] 中每个对象，最多 SHELF_MAX_BOOKS 本
    while (p < len && out.size() < (size_t)SHELF_MAX_BOOKS) {
        while (p < len) {
            char c = data[p];
            if (c==' '||c=='\t'||c=='\n'||c=='\r'||c==',') { p++; continue; }
            break;
        }
        if (p >= len) break;
        if (data[p] == ']') break;       // 数组结束
        if (data[p] != '{') { p++; continue; }
        size_t obj_start = p;
        size_t obj_end = find_matching_brace(data, len, obj_start);
        if (obj_end == (size_t)-1) return false;

        BookEntry be;
        scan_object_fields(data, len, obj_start, obj_end, [&](const char* key, size_t klen, size_t val_pos) {
            if (klen == 6 && memcmp(key, "bookId", 6) == 0)
                be.bookId = extract_str_value(data, len, val_pos);
            else if (klen == 5 && memcmp(key, "title", 5) == 0)
                be.title = extract_str_value(data, len, val_pos);
            else if (klen == 6 && memcmp(key, "author", 6) == 0)
                be.author = extract_str_value(data, len, val_pos);
            else if (klen == 5 && memcmp(key, "cover", 5) == 0)
                be.cover = extract_str_value(data, len, val_pos);
            else if (klen == 6 && memcmp(key, "format", 6) == 0)
                be.format = extract_str_value(data, len, val_pos);
        });
        if (!be.bookId.empty()) out.push_back(std::move(be));
        p = obj_end + 1;
    }

    if (out.empty()) return false;

    // 3. 定位 "bookProgress":[ 并合并进度
    int bp_arr = find_array_key(data, len, "\"bookProgress\"");
    if (bp_arr < 0) return false;  // 找不到 bookProgress → 回退 cJSON
    size_t q = (size_t)bp_arr + 1;  // 跳过 [

    size_t remaining = out.size();
    std::vector<char> matched(out.size(), 0);

    while (q < len && remaining > 0) {
        while (q < len) {
            char c = data[q];
            if (c==' '||c=='\t'||c=='\n'||c=='\r'||c==',') { q++; continue; }
            break;
        }
        if (q >= len) break;
        if (data[q] == ']') break;
        if (data[q] != '{') { q++; continue; }
        size_t obj_start = q;
        size_t obj_end = find_matching_brace(data, len, obj_start);
        if (obj_end == (size_t)-1) break;

        std::string bid;
        int progress = 0;
        std::string chapterUid;
        uint32_t updateTime = 0;
        scan_object_fields(data, len, obj_start, obj_end, [&](const char* key, size_t klen, size_t val_pos) {
            if (klen == 6 && memcmp(key, "bookId", 6) == 0) {
                bid = extract_str_value(data, len, val_pos);
            } else if (klen == 8 && memcmp(key, "progress", 8) == 0) {
                double dv;
                if (extract_num_value(data, len, val_pos, &dv)) progress = (int)dv;
            } else if (klen == 10 && memcmp(key, "chapterUid", 10) == 0) {
                if (val_pos < len && data[val_pos] == '"')
                    chapterUid = extract_str_value(data, len, val_pos);
                else {
                    double dv;
                    if (extract_num_value(data, len, val_pos, &dv))
                        chapterUid = std::to_string((long long)dv);
                }
            } else if (klen == 10 && memcmp(key, "updateTime", 10) == 0) {
                double dv;
                if (extract_num_value(data, len, val_pos, &dv)) updateTime = (uint32_t)dv;
            }
        });

        if (!bid.empty()) {
            for (size_t k = 0; k < out.size(); k++) {
                if (!matched[k] && out[k].bookId == bid) {
                    out[k].progress = progress;
                    out[k].progressChapterUid = chapterUid;
                    out[k].readUpdateTime = updateTime;
                    matched[k] = 1;
                    if (remaining > 0) remaining--;
                    break;
                }
            }
        }
        q = obj_end + 1;
    }

    return true;
}

// cJSON 全量解析（流式失败时的回退路径，保留原始实现）。
static bool parseBookshelfCjson(const char* data, size_t len, std::vector<BookEntry>& out, std::string& err) {
    cJSON* root = cJSON_ParseWithLength(data, len);
    if (!root) {
        err = "shelf JSON 解析失败";
        ESP_LOGE(TAG, "shelf cJSON_Parse 失败: len=%d free_heap=%lu free_psram=%lu",
                 (int)len,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        return false;
    }

    int ec = check_errcode(root);
    if (ec == -2012) { cJSON_Delete(root); err = "cookie 过期(-2012)"; return false; }

    cJSON* books = cJSON_GetObjectItem(root, "books");
    if (!books || !cJSON_IsArray(books)) {
        cJSON_Delete(root);
        err = "书架为空或解析无 books";
        return false;
    }

    cJSON* b = nullptr;
    int pushed = 0;
    cJSON_ArrayForEach(b, books) {
        BookEntry be;
        be.bookId = cj_strval(b, "bookId");
        be.title  = cj_strval(b, "title");
        be.author = cj_strval(b, "author");
        be.cover  = cj_strval(b, "cover");
        if (!be.bookId.empty()) { out.push_back(std::move(be)); pushed++; }
    }
    ESP_LOGI(TAG, "shelf books=%d push=%d", (int)cJSON_GetArraySize(books), pushed);

    // 合并 bookProgress[] 的阅读进度
    cJSON* bp = cJSON_GetObjectItem(root, "bookProgress");
    if (bp && cJSON_IsArray(bp)) {
        cJSON* p = nullptr;
        cJSON_ArrayForEach(p, bp) {
            std::string bid = cj_strval(p, "bookId");
            if (bid.empty()) continue;
            for (auto& be : out) {
                if (be.bookId != bid) continue;
                be.progress = cj_int(p, "progress", 0);
                cJSON* cu = cJSON_GetObjectItem(p, "chapterUid");
                if (cu) {
                    if (cJSON_IsString(cu)) be.progressChapterUid = cu->valuestring;
                    else if (cJSON_IsNumber(cu)) be.progressChapterUid = std::to_string(cu->valueint);
                }
                cJSON* ut = cJSON_GetObjectItem(p, "updateTime");
                if (ut && cJSON_IsNumber(ut)) be.readUpdateTime = (uint32_t)ut->valuedouble;
                break;
            }
        }
    }

    cJSON_Delete(root);
    if (out.empty()) { err = "书架为空或解析无 books"; return false; }
    return true;
}

bool getBookshelf(std::vector<BookEntry>& out, std::string& err) {
    out.clear();

    HttpResponse r;
    int attempt = 0;
    for (; attempt < 4; attempt++) {
        if (attempt) {
            ESP_LOGI(TAG, "shelf 重试 %d/4，等待 500ms", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        r = WR.get(std::string(WEREAD_HOST_WEB) + "/web/shelf/sync");
        if (!r.ok()) {
            err = "shelf/sync HTTP " + std::to_string(r.status) + " " + r.error;
            ESP_LOGW(TAG, "shelf HTTP 失败(try %d/4): %s", attempt + 1, err.c_str());
            continue;  // HTTP 级失败也重试，不直接返回
        }
        ESP_LOGI(TAG, "shelf 接收 len=%d (psram=%s) head=%.120s",
                 (int)r.length(), r.psbody ? "是" : "否", r.data());
        if (r.length() > 100) {
            ESP_LOGI(TAG, "shelf tail=%.100s", r.data() + r.length() - 100);
        }
        if (json_tail_complete(r)) break;
        ESP_LOGW(TAG, "shelf 响应不完整（第 %d 次）", attempt + 1);
    }
    if (!r.ok()) {
        // 所有 HTTP 尝试都失败
        shelf_debug_dump(r, err);
        return false;
    }
    if (!json_tail_complete(r)) {
        err = "shelf JSON 解析失败: IncompleteInput";
        ESP_LOGE(TAG, "shelf 4次重试仍不完整: len=%d free_heap=%lu free_psram=%lu",
                 (int)r.length(),
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        shelf_debug_dump(r, err);
        return false;
    }

    // 优先流式扫描（避免 cJSON 对 2400+ 本书全量建树导致 PSRAM 溢出）
    if (parseBookshelfStream(r.data(), r.length(), out)) {
        ESP_LOGI(TAG, "shelf 流式解析 books=%d (cap=%d) free_heap=%lu free_psram=%lu",
                 (int)out.size(), (int)SHELF_MAX_BOOKS,
                 (unsigned long)esp_get_free_heap_size(),
                 (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
        std::stable_sort(out.begin(), out.end(),
            [](const BookEntry& a, const BookEntry& b) { return a.readUpdateTime > b.readUpdateTime; });
        return true;
    }

    // 回退：cJSON 全量解析
    ESP_LOGW(TAG, "shelf 流式解析失败，回退 cJSON: len=%d", (int)r.length());
    out.clear();
    if (parseBookshelfCjson(r.data(), r.length(), out, err)) {
        std::stable_sort(out.begin(), out.end(),
            [](const BookEntry& a, const BookEntry& b) { return a.readUpdateTime > b.readUpdateTime; });
        return true;
    }
    shelf_debug_dump(r, err);
    return false;
}

// ---- 书籍信息 ----
bool getBookInfo(const std::string& bookId, BookEntry& out, std::string& err) {
    HttpResponse r = WR.get(std::string(WEREAD_HOST_I) + "/book/info?bookId=" + bookId);
    if (!r.ok()) { err = "info HTTP " + std::to_string(r.status); return false; }
    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (!root) { err = "info JSON 解析失败"; return false; }
    out.bookId = bookId;
    out.title  = cj_strval(root, "title");
    out.author = cj_strval(root, "author");
    out.cover  = cj_strval(root, "cover");
    cJSON_Delete(root);
    return true;
}

// ---- 目录 ----
bool getChapterInfos(const std::string& bookId, std::vector<ChapterEntry>& out,
                     std::string& format_out, std::string& err) {
    out.clear();
    std::string body = "{\"bookIds\":[\"" + bookId + "\"]}";
    HttpResponse r;
    int attempt = 0;
    for (; attempt < 2; attempt++) {
        r = WR.postJson(std::string(WEREAD_HOST_WEB) + "/web/book/chapterInfos", body);
        if (!r.ok()) {
            err = "chapterInfos HTTP " + std::to_string(r.status);
            ESP_LOGW(TAG, "toc HTTP 失败: %s len=%u head=%.160s", err.c_str(),
                     (unsigned)r.length(), r.data());
            return false;
        }
        if (json_tail_complete(r)) break;
        ESP_LOGW(TAG, "toc 响应不完整（第 %d 次）", attempt + 1);
    }
    if (!json_tail_complete(r)) {
        err = "chapterInfos JSON 解析失败: IncompleteInput";
        ESP_LOGW(TAG, "toc 截断: len=%u head=%.160s", (unsigned)r.length(), r.data());
        return false;
    }

    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (!root) { err = "chapterInfos JSON 解析失败"; return false; }

    int ec = check_errcode(root);
    if (ec == -2012) { cJSON_Delete(root); err = "cookie 过期(-2012)"; return false; }

    cJSON* data = cJSON_GetObjectItem(root, "data");
    if (!data || !cJSON_IsArray(data)) {
        ESP_LOGW(TAG, "toc 无 data[]: ec=%d len=%u head=%.200s", ec,
                 (unsigned)r.length(), r.data());
        cJSON_Delete(root);
        err = "chapterInfos 无 data[]";
        return false;
    }

    cJSON* item = nullptr;
    cJSON_ArrayForEach(item, data) {
        std::string bid = cj_strval(item, "bookId");
        cJSON* bookObj = cJSON_GetObjectItem(item, "book");
        std::string bookObjId = bookObj ? cj_strval(bookObj, "bookId") : std::string();
        if (bid != bookId && bookObjId != bookId) continue;
        format_out = cj_strval(item, "format");
        cJSON* updated = cJSON_GetObjectItem(item, "updated");
        if (!updated || !cJSON_IsArray(updated)) updated = cJSON_GetObjectItem(item, "chapterInfos");
        int updatedCount = (updated && cJSON_IsArray(updated)) ? cJSON_GetArraySize(updated) : 0;
        ESP_LOGI(TAG, "toc 命中 bookId=%s format=%s updated=%s count=%d",
                 bookId.c_str(), format_out.c_str(),
                 (updated && cJSON_IsArray(updated)) ? "有" : "无", updatedCount);
        if (updated && cJSON_IsArray(updated)) {
            cJSON* ch = nullptr;
            cJSON_ArrayForEach(ch, updated) {
                ChapterEntry ce;
                ce.chapterUid = cj_strval_n(ch, "chapterUid");
                ce.title      = cj_strval(ch, "title");
                ce.chapterIdx = cj_int(ch, "chapterIdx", 0);
                ce.wordCount  = cj_int(ch, "wordCount", 0);
                ce.paid       = cj_bool(ch, "paid", false);
                ce.tar        = cj_strval(ch, "tar");
                if (!ce.chapterUid.empty()) out.push_back(std::move(ce));
            }
        }
        if (out.empty()) {
            // 命中 bookId 但无章节：打印 item 原始内容诊断（可能是 EPUB/公版书走不同字段）
            char* dump = cJSON_PrintUnformatted(item);
            if (dump) {
                ESP_LOGW(TAG, "toc item 无章节: %.300s", dump);
                free(dump);
            }
            cJSON_Delete(root);
            err = "chapterInfos 命中但无章节 (format=" + format_out + ")";
            return false;
        }
        cJSON_Delete(root);
        return true;
    }
    cJSON_Delete(root);
    err = "chapterInfos 未找到 bookId=" + bookId;
    return false;
}

// ---- psvts 提取 ----
// 不依赖 \0 结尾，兼容 "psvts" / 'psvts' / psvts 三种写法
std::string extractPsvts(const char* html, size_t len) {
    if (!html || !len) return "";
    auto find_in = [&](const char* needle, size_t nlen) -> const char* {
        if (len < nlen) return nullptr;
        for (size_t i = 0; i + nlen <= len; i++) {
            if (memcmp(html + i, needle, nlen) == 0) return html + i;
        }
        return nullptr;
    };
    const char* end = html + len;
    const char* idx = find_in("\"psvts\"", 7);
    if (!idx) idx = find_in("'psvts'", 7);
    if (!idx) idx = find_in("psvts", 5);
    if (!idx) return "";
    const char* colon = (const char*)memchr(idx, ':', end - idx);
    if (!colon) return "";
    // 跳过冒号与空白
    const char* p = colon + 1;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
    if (p >= end) return "";
    char quote = 0;
    if (*p == '"' || *p == '\'') { quote = *p; p++; }
    if (!quote) return "";  // psvts 必须是字符串字面量
    const char* q2 = (const char*)memchr(p, quote, end - p);
    if (!q2) return "";
    return std::string(p, (size_t)(q2 - p));
}

std::string extractPsvts(const std::string& html) {
    return extractPsvts(html.data(), html.size());
}

// reader URL
static std::string reader_url(const std::string& bookId, const std::string& chapterUid) {
    std::string u = std::string(WEREAD_HOST_WEB) + "/web/reader/" + weread::e(bookId);
    if (!chapterUid.empty()) u += "k" + weread::e(chapterUid);
    return u;
}

// 从 params 列表构造 JSON body
static std::string params_to_json(const std::vector<std::pair<std::string,std::string>>& p) {
    std::string j = "{";
    bool first = true;
    for (auto& kv : p) {
        if (!first) j += ",";
        // 数字字段不加引号：ct, sc, st, r
        bool numeric = (kv.first == "ct" || kv.first == "sc" || kv.first == "st" || kv.first == "r");
        j += "\"" + kv.first + "\":";
        if (numeric) j += kv.second;
        else { j += "\""; j += kv.second; j += "\""; }
        first = false;
    }
    j += "}";
    return j;
}

// ---- XHTML 去标签（指针版，直接解析 PSRAM 大数据）----
static const size_t TEXT_BLOCK_MAX = 48 * 1024;

template <typename Emit>
static void strip_and_emit(const char* p, size_t n, Emit emit) {
    std::string t;
    int nl = 0;        // 连续换行计数（空行折叠）
    bool in_tag = false;
    auto putc = [&](char c) {
        if (c == '\n') { nl++; if (nl <= 1) t += '\n'; }
        else { nl = 0; t += c; }
    };
    auto flush = [&]() {
        size_t cut = t.size();
        size_t br = t.rfind('\n');
        if (br != std::string::npos && br > TEXT_BLOCK_MAX / 2) cut = br + 1;
        else cut = utf8_safe_cut(t.data(), TEXT_BLOCK_MAX);
        if (!cut) cut = 1;
        std::string piece = trim(t.substr(0, cut));
        if (!piece.empty()) emit(piece);
        t = t.substr(cut);
        nl = 0;
    };
    for (size_t i = 0; i < n; i++) {
        char c = p[i];
        if (c == '<') {
            size_t rem = n - i;
            if ((rem >= 2 && p[i+1] == 'p') ||
                (rem >= 3 && p[i+1] == 'b' && p[i+2] == 'r') ||
                (rem >= 3 && p[i+1] == 'h' && p[i+2] == '1') ||
                (rem >= 4 && p[i+1] == '/' && p[i+2] == 'p' && p[i+3] == '>') ||
                (rem >= 5 && p[i+1] == '/' && p[i+2] == 'h' && p[i+3] == '1' && p[i+4] == '>')) {
                putc('\n');
            }
            in_tag = true;
            continue;
        }
        if (c == '>') { in_tag = false; continue; }
        if (!in_tag) {
            if (c == '&') {
                int semi = find_sub(p, n, i, ";", 1);
                if (semi > 0 && semi - (int)i < 8) {
                    std::string ent(p + i, (size_t)(semi - i + 1));
                    if (ent == "&amp;") putc('&');
                    else if (ent == "&lt;") putc('<');
                    else if (ent == "&gt;") putc('>');
                    else if (ent == "&quot;") putc('"');
                    else if (ent == "&nbsp;") putc(' ');
                    else if (ent == "&apos;") putc('\'');
                    else { for (int k = (int)i; k <= semi; k++) putc(p[k]); }
                    i = semi;
                    if (t.size() >= TEXT_BLOCK_MAX) flush();
                    continue;
                }
            }
            putc(c);
        }
        if (t.size() >= TEXT_BLOCK_MAX) flush();
    }
    {
        std::string piece = trim(t);
        if (!piece.empty()) emit(piece);
    }
}

static std::string strip_xhtml(const char* p, size_t n) {
    std::string out;
    strip_and_emit(p, n, [&](const std::string& piece) {
        if (!out.empty()) out += '\n';
        out += piece;
    });
    return trim(out);
}

// ---- 正文下载专用 keep-alive 会话 ----
struct ShardRespBuf {
    char* psbuf = nullptr;
    size_t pslen = 0;
    size_t pscap = 0;
    bool overflow = false;
    static const size_t INIT = 256 * 1024;
    static const size_t CAP  = 2 * 1024 * 1024;
};

static esp_err_t shard_http_event(esp_http_client_event_t* evt) {
    ShardRespBuf* sr = (ShardRespBuf*)evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && sr && evt->data_len > 0) {
        size_t need = sr->pslen + evt->data_len;
        if (need > sr->pscap) {
            size_t newcap = sr->pscap ? sr->pscap : ShardRespBuf::INIT;
            while (newcap < need && newcap < ShardRespBuf::CAP) newcap *= 2;
            if (newcap > ShardRespBuf::CAP) newcap = ShardRespBuf::CAP;
            if (newcap >= need) {
                char* nb = (char*)heap_caps_realloc(sr->psbuf, newcap, MALLOC_CAP_SPIRAM);
                if (nb) { sr->psbuf = nb; sr->pscap = newcap; }
                else ESP_LOGE(TAG, "shard PSRAM 扩到 %u KB 失败", (unsigned)(newcap / 1024));
            }
        }
        if (sr->pslen + evt->data_len <= sr->pscap) {
            memcpy(sr->psbuf + sr->pslen, evt->data, evt->data_len);
            sr->pslen += evt->data_len;
        } else {
            sr->overflow = true;
        }
    }
    return ESP_OK;
}

volatile bool g_net_cancel = false;
void (*chapter_stage_cb)(const char* stage, int cur, int total) = nullptr;
static inline void stage_tick(const char* s, int c, int t) { if (chapter_stage_cb) chapter_stage_cb(s, c, t); }

struct ShardSession {
    esp_http_client_handle_t client = nullptr;
    ShardRespBuf rb;
    int64_t last_use = 0;
    int64_t last_req = 0;
    int in_use = 0;  // RAII 计数（单线程网络任务中使用，无需原子）

    void pace() {
        int64_t now = millis_now();
        if (last_req && now - last_req < 400) {
            int ms = 400 - (int)(now - last_req);
            if (ms > 0) vTaskDelay(pdMS_TO_TICKS(ms));
        }
        last_req = millis_now();
    }

    bool open() {
        if (client) return true;
        esp_http_client_config_t cfg = {};
        cfg.host = "weread.qq.com";
        cfg.path = "/";
        cfg.transport_type = HTTP_TRANSPORT_OVER_SSL;
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
        cfg.timeout_ms = HTTP_TIMEOUT_MS;
        cfg.buffer_size = 4096;
        cfg.buffer_size_tx = 2048;
        cfg.keep_alive_enable = true;
        cfg.event_handler = shard_http_event;
        cfg.user_data = &rb;
        client = esp_http_client_init(&cfg);
        if (!client && rb.psbuf) { heap_caps_free(rb.psbuf); rb.psbuf = nullptr; rb.pscap = 0; }
        return client != nullptr;
    }

    void close() {
        if (client) { esp_http_client_cleanup(client); client = nullptr; }
    }

    char* request(bool post, const std::string& path, const std::string* body,
                  const std::string& referer, size_t& out_len, int& status) {
        out_len = 0;
        status = -1;
        struct UseGuard {
            int& c; UseGuard(int& c_) : c(c_) { c++; } ~UseGuard() { c--; }
        } guard(in_use);
        pace();
        if (!open()) return nullptr;
        if (!rb.psbuf) {
            rb.psbuf = (char*)heap_caps_malloc(ShardRespBuf::INIT, MALLOC_CAP_SPIRAM);
            if (!rb.psbuf) { ESP_LOGE(TAG, "shard PSRAM 分配失败"); return nullptr; }
            rb.pscap = ShardRespBuf::INIT;
        }
        rb.pslen = 0;
        rb.overflow = false;
        esp_http_client_set_url(client, path.c_str());
        esp_http_client_set_method(client, post ? HTTP_METHOD_POST : HTTP_METHOD_GET);
        esp_http_client_set_header(client, "User-Agent", WEREAD_USER_AGENT);
        esp_http_client_set_header(client, "Accept", "application/json, text/plain, */*");
        esp_http_client_set_header(client, "Origin", "https://weread.qq.com");
        esp_http_client_set_header(client, "Referer", referer.c_str());
        std::string cookie = WR.cookieHeader();
        if (!cookie.empty()) esp_http_client_set_header(client, "Cookie", cookie.c_str());
        if (post) {
            esp_http_client_set_header(client, "Content-Type", "application/json;charset=UTF-8");
            esp_http_client_set_post_field(client, body->data(), (int)body->size());
        } else {
            esp_http_client_set_post_field(client, nullptr, 0);
        }
        esp_err_t e = esp_http_client_perform(client);
        if (e != ESP_OK) {
            ESP_LOGW(TAG, "shard perform 失败 err=%s path=%s", esp_err_to_name(e), path.c_str());
            return nullptr;
        }
        status = esp_http_client_get_status_code(client);
        last_use = millis_now();
        if (rb.overflow) {
            ESP_LOGW(TAG, "shard 响应超 %u KB 上限被截断 %s",
                     (unsigned)(ShardRespBuf::CAP / 1024), path.c_str());
            rb.overflow = false;
            return nullptr;
        }
        // 完整性校验：比较 Content-Length 与实际接收长度
        // TLS 读取错误（errno=ENOMEM 等）可能导致 perform 返回 OK 但数据截断
        // chunked 传输时 content_length 返回 -1，跳过校验
        int64_t clen = esp_http_client_get_content_length(client);
        if (clen > 0 && (int64_t)rb.pslen < clen) {
            ESP_LOGW(TAG, "shard 响应不完整: 收到 %u / 期望 %lld bytes, path=%s",
                     (unsigned)rb.pslen, (long long)clen, path.c_str());
            return nullptr;
        }
        char* out = rb.psbuf;
        size_t cap = rb.pscap;
        rb.psbuf = nullptr; rb.pscap = 0;
        out_len = rb.pslen;
        if (out_len < cap) out[out_len] = 0;
        return out;
    }

    char* request_retry(bool post, const std::string& path, const std::string* body,
                        const std::string& referer, size_t& out_len, int& status) {
        if (g_net_cancel) { status = -2; return nullptr; }
        char* d = request(post, path, body, referer, out_len, status);
        if (d) return d;
        if (g_net_cancel) { status = -2; return nullptr; }
        close();
        ESP_LOGW(TAG, "shard 连接失败，丢弃重建重试");
        return request(post, path, body, referer, out_len, status);
    }
};

static ShardSession g_shard_sess;

void housekeeping() {
    if (g_shard_sess.client && g_shard_sess.in_use == 0 &&
        millis_now() - g_shard_sess.last_use > 30000) {
        ESP_LOGI(TAG, "shard 会话闲置 30s，关闭释放内存");
        g_shard_sess.close();
    }
}

// psvts 缓存（按 bookId）
static std::string g_psvts_book;
static std::string g_psvts;

// 整章时间预算
static int64_t g_chapter_budget_ms = 90000;
static int64_t g_chapter_deadline = 0;

static inline bool chapter_over_budget() {
    return g_chapter_deadline && (millis_now() - g_chapter_deadline) > 0;
}
static inline bool chapter_aborted(std::string& err) {
    if (g_net_cancel) { err = "已取消"; return true; }
    if (chapter_over_budget()) { err = "整章超时（90s 预算）"; return true; }
    return false;
}

// 章节内容形态
enum ChapterKind {
    CHAPTER_TXT,
    CHAPTER_XHTML,
    CHAPTER_EPUB_ZIP,
};

struct ChapterRaw {
    ChapterKind kind = CHAPTER_XHTML;
    char* data = nullptr;
    size_t len = 0;
    void release() { if (data) { heap_caps_free(data); data = nullptr; len = 0; } }
};

static bool fetch_chapter_raw(const std::string& bookId, const ChapterEntry& ch,
                              ChapterRaw& raw, std::string& err) {
    g_chapter_deadline = millis_now() + g_chapter_budget_ms;
    g_net_cancel = false;

    std::string rurl = reader_url(bookId, ch.chapterUid);
    std::string rpath = "/web/reader/" + weread::e(bookId);
    if (!ch.chapterUid.empty()) rpath += "k" + weread::e(ch.chapterUid);
    int status = 0;

    // 1. psvts：按 bookId 缓存
    std::string psvts;
    bool from_cache = (g_psvts_book == bookId && !g_psvts.empty());
    if (from_cache) psvts = g_psvts;
    if (psvts.empty()) {
        if (chapter_aborted(err)) return false;
        stage_tick("reader", 1, 4);
        size_t hlen = 0;
        char* html = g_shard_sess.request_retry(false, rpath, nullptr, rurl, hlen, status);
        if (!html) { err = "reader 请求失败"; return false; }
        if (status < 200 || status >= 300) { heap_caps_free(html); err = "reader HTTP " + std::to_string(status); return false; }
        psvts = extractPsvts(html, hlen);
        if (psvts.empty()) {
            ESP_LOGE(TAG, "reader psvts 提取失败 hlen=%u path=%s head=%.100s",
                     (unsigned)hlen, rpath.c_str(), hlen ? html : "(空)");
            // 把 hlen + head 前 80 字符放进 err，便于从屏幕直接诊断根因
            // （会员书未解锁/cookie 失效/被反爬时 reader HTML 不含 psvts）
            char diag[200];
            snprintf(diag, sizeof(diag), "psvts失败 hlen=%u head:%.80s",
                     (unsigned)hlen, hlen ? html : "(空)");
            heap_caps_free(html);
            err = diag;
            return false;
        }
        heap_caps_free(html);
        g_psvts_book = bookId;
        g_psvts = psvts;
    }

    // 2. e_0
    auto shard_body = [&]() -> std::string {
        auto params = weread::make_content_params(bookId, ch.chapterUid, psvts, false, 1);
        return params_to_json(params);
    };
    std::string e0_body = shard_body();
    const std::string e0_path = "/web/book/chapter/e_0";
    char* d0 = nullptr;
    size_t n0 = 0;
    auto post_e0 = [&]() {
        stage_tick("e_0", 2, 4);
        d0 = g_shard_sess.request_retry(true, e0_path, &e0_body, rurl, n0, status);
    };
    if (chapter_aborted(err)) return false;
    post_e0();
    if (!d0) { err = "e_0 请求失败"; return false; }
    auto e0_failed = [&]() {
        return status < 200 || status >= 300 || (n0 == 2 && d0[0] == '{' && d0[1] == '}');
    };
    if (e0_failed() && from_cache) {
        heap_caps_free(d0);
        d0 = nullptr;
        g_psvts.clear();
        g_psvts_book.clear();
        ESP_LOGW(TAG, "shard 缓存 psvts 失效，重取重试");
        size_t hlen = 0;
        char* html = g_shard_sess.request_retry(false, rpath, nullptr, rurl, hlen, status);
        if (!html || status < 200 || status >= 300) {
            if (html) heap_caps_free(html);
            err = "reader 重取失败 HTTP " + std::to_string(status);
            return false;
        }
        psvts = extractPsvts(html, hlen);
        heap_caps_free(html);
        if (psvts.empty()) {
            char diag[200];
            snprintf(diag, sizeof(diag), "重取psvts失败 hlen=%u", (unsigned)hlen);
            err = diag;
            return false;
        }
        g_psvts_book = bookId;
        g_psvts = psvts;
        e0_body = shard_body();
        post_e0();
        if (!d0) { err = "e_0 重试失败"; return false; }
    }
    if (e0_failed()) {
        err = "e_0 HTTP " + std::to_string(status);
        heap_caps_free(d0);
        return false;
    }

    // 形态 B：整本 EPUB zip（公版/原版书），保留 zip 数据待后续 miniz 解包
    if (n0 >= 4 && d0[0] == 'P' && d0[1] == 'K' && d0[2] == 3 && d0[3] == 4) {
        raw.data = d0;
        raw.len = n0;
        raw.kind = CHAPTER_EPUB_ZIP;
        return true;
    }

    // 形态 A：JSON → TXT 书
    bool has_bookid = false;
    if (n0 >= 8) {
        for (size_t i = 0; i + 8 <= n0; i++)
            if (memcmp(d0 + i, "\"bookId\"", 8) == 0) { has_bookid = true; break; }
    }
    bool is_txt = (n0 && d0[0] == '{' && has_bookid) || n0 == 0;

    auto post_shard = [&](const char* path, char*& out, size_t& out_len) {
        std::string body = shard_body();
        out = g_shard_sess.request_retry(true, std::string(path), &body, rurl, out_len, status);
    };

    if (is_txt) {
        char* t0 = nullptr; size_t tn0 = 0;
        char* t1 = nullptr; size_t tn1 = 0;
        if (chapter_aborted(err)) { heap_caps_free(d0); return false; }
        stage_tick("t_0", 3, 4);
        post_shard("/web/book/chapter/t_0", t0, tn0);
        if (chapter_aborted(err)) { heap_caps_free(d0); if (t0) heap_caps_free(t0); return false; }
        stage_tick("t_1", 4, 4);
        post_shard("/web/book/chapter/t_1", t1, tn1);
        heap_caps_free(d0);
        d0 = nullptr;
        if (tn0 == 2 && t0 && t0[0] == '{' && t0[1] == '}') {
            heap_caps_free(t0);
            if (t1) heap_caps_free(t1);
            err = "t_0 返回 {}（可能无权限/风控）";
            return false;
        }
        raw.data = weread::decode_content_shards_psram(t0, tn0, t1, tn1, nullptr, 0, &raw.len);
        if (t0) heap_caps_free(t0);
        if (t1) heap_caps_free(t1);
        if (!raw.data) { err = "TXT 解码失败"; return false; }
        raw.kind = CHAPTER_TXT;
        return true;
    }

    // 形态 C：常规 EPUB 分片 e_0+e_1+e_3
    char* e1 = nullptr; size_t en1 = 0;
    char* e3 = nullptr; size_t en3 = 0;
    if (chapter_aborted(err)) { heap_caps_free(d0); return false; }
    stage_tick("e_1", 3, 4);
    post_shard("/web/book/chapter/e_1", e1, en1);
    if (chapter_aborted(err)) { heap_caps_free(d0); if (e1) heap_caps_free(e1); return false; }
    stage_tick("e_3", 4, 4);
    post_shard("/web/book/chapter/e_3", e3, en3);
    raw.data = weread::decode_content_shards_psram(d0, n0, e1, en1, e3, en3, &raw.len);
    heap_caps_free(d0);
    if (e1) heap_caps_free(e1);
    if (e3) heap_caps_free(e3);
    if (!raw.data) { err = "EPUB 解码失败"; return false; }
    if (raw.len >= 4 && raw.data[0] == 'P' && raw.data[1] == 'K' && raw.data[2] == 3 && raw.data[3] == 4)
        raw.kind = CHAPTER_EPUB_ZIP;
    else
        raw.kind = CHAPTER_XHTML;
    return true;
}

// ---- EPUB 整本 zip 解包 ----
// 用 miniz 遍历 zip 内 .xhtml/.html 文件，解压后调用已有 strip_and_emit 去标签提取文本。

// 判断文件名是否为 XHTML/HTML 正文
static bool epub_zip_is_html(const char* name) {
    size_t nl = strlen(name);
    if (nl >= 6 && memcmp(name + nl - 6, ".xhtml", 6) == 0) return true;
    if (nl >= 5 && memcmp(name + nl - 5, ".html", 5) == 0) return true;
    if (nl >= 4 && memcmp(name + nl - 4, ".htm", 4) == 0) return true;
    return false;
}

// EPUB 整本 zip → 纯文本（拼接所有 xhtml 章节去标签后的文本）
static std::string epub_zip_to_text(const char* zip_data, size_t zip_len) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, zip_len, 0)) {
        ESP_LOGE(TAG, "EPUB zip 初始化失败");
        return "";
    }
    std::string out;
    mz_uint num = mz_zip_reader_get_num_files(&zip);
    ESP_LOGI(TAG, "EPUB zip 解包: %u 个文件", (unsigned)num);
    for (mz_uint i = 0; i < num; i++) {
        mz_zip_archive_file_stat zstat;
        if (!mz_zip_reader_file_stat(&zip, i, &zstat)) continue;
        if (!epub_zip_is_html(zstat.m_filename) || zstat.m_uncomp_size == 0) continue;
        // 解压单文件到内存缓冲区
        size_t uncomp = (size_t)zstat.m_uncomp_size;
        char* buf = (char*)malloc(uncomp);
        if (!buf) { ESP_LOGW(TAG, "EPUB 解压内存不足: %s (%uB)", zstat.m_filename, (unsigned)uncomp); continue; }
        if (!mz_zip_reader_extract_to_mem(&zip, i, buf, uncomp, 0)) {
            free(buf);
            ESP_LOGW(TAG, "EPUB 文件解压失败: %s", zstat.m_filename);
            continue;
        }
        // 复用已有的 strip_xhtml 去标签提取文本
        std::string part = strip_xhtml(buf, uncomp);
        if (!part.empty()) {
            if (!out.empty()) out += '\n';
            out += part;
        }
        free(buf);
    }
    mz_zip_reader_end(&zip);
    return trim(out);
}

// ---- 拉取单章纯文本 ----
bool getChapterText(const std::string& bookId, const ChapterEntry& ch,
                    std::string& text_out, std::string& err) {
    ChapterRaw raw;
    if (!fetch_chapter_raw(bookId, ch, raw, err)) return false;
    if (raw.kind == CHAPTER_EPUB_ZIP) {
        text_out = epub_zip_to_text(raw.data, raw.len);
        raw.release();
        if (text_out.empty()) { err = "EPUB zip 解包为空"; return false; }
        return true;
    }
    if (raw.kind == CHAPTER_TXT) text_out.assign(raw.data, raw.len);
    else text_out = strip_xhtml(raw.data, raw.len);
    raw.release();
    return true;
}

// ---- 上传阅读进度 ----
static const char* READER_TOKEN = "3c5c8717f3daf09iop3423zafeqoi";

static std::string sha256_hex(const std::string& s) {
    uint8_t out[32];
    mbedtls_sha256((const uint8_t*)s.data(), s.size(), out, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[64] = 0;
    return std::string(hex);
}

static std::string web_app_id() {
    std::string ua = WEREAD_USER_AGENT;
    std::string prefix;
    int count = 0;
    size_t start = 0;
    for (size_t i = 0; i <= ua.size() && count < 12; i++) {
        if (i == ua.size() || ua[i] == ' ' || ua[i] == '\t') {
            if (i > start) { prefix += std::to_string((i - start) % 10); count++; }
            start = i + 1;
        }
    }
    uint32_t hash = 0;
    for (size_t i = 0; i < ua.size(); i++)
        hash = (uint32_t)((0x83ULL * hash + (uint8_t)ua[i]) & 0x7fffffff);
    return "wb" + prefix + "h" + std::to_string(hash);
}

bool uploadProgress(const std::string& bookId, const std::string& chapterUid,
                    int chapterIdx, int progressPercent, std::string& err) {
    // 1. 拿 reader HTML 提取 psvts
    std::string rurl = reader_url(bookId, chapterUid);
    HttpResponse rh = WR.get(rurl, rurl);
    if (!rh.ok()) { err = "reader HTTP " + std::to_string(rh.status); return false; }
    std::string psvts = extractPsvts(rh.data(), rh.length());
    if (psvts.empty()) { err = "未提取到 psvts"; return false; }

    // 2. 构造签名 payload
    long ct = (long)time(nullptr);
    char ts_buf[24];
    snprintf(ts_buf, sizeof(ts_buf), "%llu",
             (unsigned long long)ct * 1000 + (esp_random() % 1000));
    std::string ts = ts_buf;
    std::string rn = std::to_string(esp_random() % 1000);

    std::vector<std::pair<std::string,std::string>> p;
    p.push_back({"appId", web_app_id()});
    p.push_back({"b", weread::e(bookId)});
    p.push_back({"c", weread::e(chapterUid)});
    p.push_back({"ci", std::to_string(chapterIdx)});
    p.push_back({"co", "0"});
    p.push_back({"sm", ""});
    p.push_back({"pr", std::to_string(progressPercent)});
    p.push_back({"rt", "0"});
    p.push_back({"ts", ts});
    p.push_back({"rn", rn});
    p.push_back({"sg", sha256_hex(ts + rn + READER_TOKEN)});
    p.push_back({"ct", std::to_string(ct)});
    p.push_back({"ps", psvts});
    p.push_back({"pc", weread::e(std::to_string(ct))});
    std::string q;
    weread::sorted_query(p, q);
    p.push_back({"s", weread::sign(q)});

    // 3. 序列化 JSON body
    auto is_num = [](const std::string& k) {
        return k == "ci" || k == "co" || k == "pr" || k == "rt" || k == "ts" || k == "rn" || k == "ct";
    };
    std::string body = "{";
    bool first = true;
    for (auto& kv : p) {
        if (!first) body += ",";
        body += "\"" + kv.first + "\":";
        if (is_num(kv.first)) body += kv.second;
        else { body += "\""; body += kv.second; body += "\""; }
        first = false;
    }
    body += "}";

    HttpResponse r = WR.postJson(std::string(WEREAD_HOST_WEB) + "/web/book/read", body, rurl);
    if (!r.ok()) { err = "read HTTP " + std::to_string(r.status); return false; }
    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (!root) { err = "read JSON 解析失败"; return false; }
    cJSON* succ = cJSON_GetObjectItem(root, "succ");
    bool ok = (succ && cJSON_IsNumber(succ) && succ->valueint == 1);
    cJSON_Delete(root);
    if (!ok) {
        size_t n = std::min(r.length(), (size_t)120);
        err = "进度上传被拒: " + std::string(r.data(), n);
        return false;
    }
    return true;
}

// ---- 书籍详情 ----
bool getBookDetail(const std::string& bookId, BookDetail& out, std::string& err) {
    HttpResponse r = WR.get(std::string(WEREAD_HOST_WEB) + "/web/book/info?bookId=" + bookId);
    if (!r.ok()) { err = "book/info HTTP " + std::to_string(r.status); return false; }

    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (!root) { err = "book/info JSON 解析失败"; return false; }
    int ec = check_errcode(root);
    if (ec == -2012) { cJSON_Delete(root); err = "cookie 过期(-2012)"; return false; }
    if (!cJSON_GetObjectItem(root, "title")) {
        cJSON_Delete(root);
        err = "book/info 无 title（bookId 无效?）";
        return false;
    }
    out.title       = cj_strval(root, "title");
    out.author      = cj_strval(root, "author");
    out.intro       = cj_strval(root, "intro");
    out.cover       = cj_strval(root, "cover");
    out.publisher   = cj_strval(root, "publisher");
    out.authorIntro = "";
    cJSON_Delete(root);
    return true;
}

// ---- 热门评论 ----
static std::string utf8_prefix(const std::string& s, size_t max_chars) {
    size_t i = 0, n = 0;
    while (i < s.size() && n < max_chars) {
        uint8_t c = (uint8_t)s[i];
        size_t len = 1;
        if (c >= 0xF0) len = 4;
        else if (c >= 0xE0) len = 3;
        else if (c >= 0xC0) len = 2;
        i += len;
        n++;
    }
    return s.substr(0, i);
}

bool getHotReviews(const std::string& bookId, std::vector<Review>& out,
                   int count, std::string& err) {
    out.clear();
    if (count <= 0) count = 3;
    std::string url = std::string(WEREAD_HOST_WEB) + "/web/review/list?bookId=" + bookId +
                     "&listType=3&maxIdx=0&count=" + std::to_string(count);
    HttpResponse r = WR.get(url);
    if (!r.ok()) { err = "review/list HTTP " + std::to_string(r.status); return false; }

    cJSON* root = cJSON_ParseWithLength(r.data(), r.length());
    if (!root) { err = "review/list JSON 解析失败"; return false; }
    int ec = check_errcode(root);
    if (ec != 0) { cJSON_Delete(root); err = "review/list errCode=" + std::to_string(ec); return false; }

    cJSON* reviews = cJSON_GetObjectItem(root, "reviews");
    if (reviews && cJSON_IsArray(reviews)) {
        cJSON* rv = nullptr;
        cJSON_ArrayForEach(rv, reviews) {
            cJSON* inner = cJSON_GetObjectItem(rv, "review");
            if (!inner) continue;
            Review rev;
            cJSON* author = cJSON_GetObjectItem(inner, "author");
            if (author) rev.nick = cj_strval(author, "name");
            rev.content = utf8_prefix(cj_strval(inner, "content"), 200);
            rev.likes = 0;
            if (!rev.content.empty()) out.push_back(std::move(rev));
        }
    }
    cJSON_Delete(root);
    if (out.empty()) { err = "本书暂无热门评论"; return false; }
    return true;
}

// ---- 章节图文分块 ----
static std::string normalize_image_url(const std::string& src) {
    if (src.compare(0, 7, "http://") == 0 || src.compare(0, 8, "https://") == 0) return src;
    if (src.compare(0, 2, "//") == 0) return "https:" + src;
    if (!src.empty() && src[0] == '/') return std::string(WEREAD_HOST_WEB) + src;
    return std::string(WEREAD_HOST_WEB) + "/" + src;
}

static std::string extract_img_src(const char* p, size_t n) {
    for (size_t i = 0; i + 3 <= n; i++) {
        if (p[i] != 's' || p[i+1] != 'r' || p[i+2] != 'c') continue;
        size_t j = i + 3;
        while (j < n && (p[j] == ' ' || p[j] == '=')) j++;
        if (j >= n) return "";
        char quo = p[j];
        if (quo != '"' && quo != '\'') return "";
        size_t e = j + 1;
        while (e < n && p[e] != quo) e++;
        if (e >= n) return "";
        return std::string(p + j + 1, e - j - 1);
    }
    return "";
}

static void xhtml_to_blocks(const char* raw, size_t rawlen,
                            const std::string& epub_dir, std::vector<ContentBlock>& out) {
    auto push_text = [&](const char* p, size_t n, uint8_t style) {
        strip_and_emit(p, n, [&](const std::string& piece) {
            ContentBlock b;
            b.is_image = false;
            b.style = style;
            b.text = piece;
            out.push_back(std::move(b));
        });
    };

    size_t pos = 0;
    for (;;) {
        int im = find_sub(raw, rawlen, pos, "<img", 4);
        while (im >= 0 && (size_t)im + 4 < rawlen &&
               raw[im + 4] != ' ' && raw[im + 4] != '\t' && raw[im + 4] != '\r' &&
               raw[im + 4] != '\n' && raw[im + 4] != '/') {
            im = find_sub(raw, rawlen, im + 4, "<img", 4);
        }
        int ht = -1; uint8_t lvl = 0;
        for (uint8_t k = 1; k <= 4; k++) {
            char pat[4]  = {'<', 'h', (char)('0' + k), '>'};
            char pat2[4] = {'<', 'h', (char)('0' + k), ' '};
            int t = find_sub(raw, rawlen, pos, pat, 4);
            if (t < 0) t = find_sub(raw, rawlen, pos, pat2, 4);
            if (t >= 0 && (ht < 0 || t < ht)) { ht = t; lvl = k; }
        }
        if (im < 0 && ht < 0) break;
        int next;
        bool is_img;
        if (im >= 0 && (ht < 0 || im < ht)) { next = im; is_img = true; }
        else { next = ht; is_img = false; }
        push_text(raw + pos, (size_t)next - pos, 0);

        if (is_img) {
            int gt = find_sub(raw, rawlen, im, ">", 1);
            if (gt < 0) break;
            std::string src = extract_img_src(raw + im, (size_t)gt + 1 - im);
            if (!src.empty()) {
                bool absolute = src.compare(0, 7, "http://") == 0 ||
                                src.compare(0, 8, "https://") == 0 ||
                                src.compare(0, 2, "//") == 0;
                if (epub_dir.empty() && !absolute) {
                    // 分片章节的相对路径装饰图：跳过
                } else if (!absolute) {
                    // EPUB 包内图片：暂不支持，跳过
                } else {
                    ContentBlock b;
                    b.is_image = true;
                    b.text = normalize_image_url(src);
                    out.push_back(std::move(b));
                }
            }
            pos = gt + 1;
        } else {
            int gt = find_sub(raw, rawlen, next, ">", 1);
            if (gt < 0) break;
            char close[5] = {'<', '/', 'h', (char)('0' + lvl), '>'};
            int ce = find_sub(raw, rawlen, gt + 1, close, 5);
            if (ce < 0) { pos = gt + 1; continue; }
            int h_im = find_sub(raw, rawlen, gt + 1, "<img", 4);
            if (h_im >= 0 && h_im < ce) {
                push_text(raw + gt + 1, (size_t)h_im - (gt + 1), lvl);
                pos = h_im;
                continue;
            }
            push_text(raw + gt + 1, (size_t)ce - (gt + 1), lvl);
            pos = ce + 5;
        }
    }
    push_text(raw + pos, rawlen - pos, 0);
}

// EPUB 整本 zip → 图文块（逐个 xhtml 调用 xhtml_to_blocks 切块）
static bool epub_zip_to_blocks(const char* zip_data, size_t zip_len,
                                std::vector<ContentBlock>& out) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zip_data, zip_len, 0)) {
        ESP_LOGE(TAG, "EPUB zip 初始化失败");
        return false;
    }
    mz_uint num = mz_zip_reader_get_num_files(&zip);
    ESP_LOGI(TAG, "EPUB zip 解包: %u 个文件", (unsigned)num);
    for (mz_uint i = 0; i < num; i++) {
        mz_zip_archive_file_stat zstat;
        if (!mz_zip_reader_file_stat(&zip, i, &zstat)) continue;
        if (!epub_zip_is_html(zstat.m_filename) || zstat.m_uncomp_size == 0) continue;
        size_t uncomp = (size_t)zstat.m_uncomp_size;
        char* buf = (char*)malloc(uncomp);
        if (!buf) { ESP_LOGW(TAG, "EPUB 解压内存不足: %s (%uB)", zstat.m_filename, (unsigned)uncomp); continue; }
        if (!mz_zip_reader_extract_to_mem(&zip, i, buf, uncomp, 0)) {
            free(buf);
            continue;
        }
        // 推算文件所在目录作为 epub_dir（图片相对路径前缀）
        std::string dir;
        const char* slash = strrchr(zstat.m_filename, '/');
        if (slash) dir.assign(zstat.m_filename, slash - zstat.m_filename);
        xhtml_to_blocks(buf, uncomp, dir, out);
        free(buf);
    }
    mz_zip_reader_end(&zip);
    return !out.empty();
}

bool getChapterBlocks(const std::string& bookId, const ChapterEntry& ch,
                      std::vector<ContentBlock>& out, std::string& err) {
    out.clear();
    ChapterRaw raw;
    if (!fetch_chapter_raw(bookId, ch, raw, err)) return false;

    // TXT 书：整章文本按 ≤48KB 切连续文本块
    if (raw.kind == CHAPTER_TXT) {
        size_t off = 0;
        while (off < raw.len) {
            size_t n = raw.len - off;
            if (n > TEXT_BLOCK_MAX) n = utf8_safe_cut(raw.data + off, TEXT_BLOCK_MAX);
            if (!n) n = 1;
            ContentBlock b;
            b.is_image = false;
            b.text.assign(raw.data + off, n);
            out.push_back(std::move(b));
            off += n;
        }
        raw.release();
        if (out.empty()) { err = "章节为空"; return false; }
        return true;
    }

    if (raw.kind == CHAPTER_EPUB_ZIP) {
        bool ok = epub_zip_to_blocks(raw.data, raw.len, out);
        raw.release();
        if (!ok) { err = "EPUB zip 解包无内容"; return false; }
        return true;
    }

    // 常规 EPUB 分片：单章 XHTML 直接切块
    xhtml_to_blocks(raw.data, raw.len, "", out);
    raw.release();
    if (out.empty()) { err = "章节无内容块"; return false; }
    return true;
}

} // namespace weread_api
