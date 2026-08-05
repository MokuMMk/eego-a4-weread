// weread_crypto.cpp — 实现
// 纯 ESP-IDF：std::string 替代 Arduino String，mbedtls 提供 MD5/base64。
#include "weread_crypto.h"
#include "mbedtls/md5.h"
#include "mbedtls/base64.h"
#include "esp_heap_caps.h"
#include "esp_random.h"  // esp_random

#include <vector>
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <ctime>

namespace weread {

// ---- 字符串辅助（替代 Arduino String 的部分方法）----

static std::string to_lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    return s;
}

static std::string to_upper(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return s;
}

std::string md5_hex(const uint8_t* data, size_t len) {
    uint8_t out[16];
    mbedtls_md5(data, len, out);
    char hex[33];
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", out[i]);
    hex[32] = 0;
    return std::string(hex);
}

std::string url_encode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    char buf[4];
    for (size_t i = 0; i < s.size(); i++) {
        unsigned char c = (unsigned char)s[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out += (char)c;
        } else {
            snprintf(buf, sizeof(buf), "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

static bool is_digit_string(const std::string& s) {
    if (s.empty()) return false;
    for (size_t i = 0; i < s.size(); i++) if (!std::isdigit((unsigned char)s[i])) return false;
    return true;
}

// 逐字节 hex（无填充）
static std::string byte_hex(const std::string& s) {
    std::string out;
    out.reserve(s.size() * 2);
    char buf[3];
    for (size_t i = 0; i < s.size(); i++) {
        snprintf(buf, sizeof(buf), "%x", (unsigned char)s[i]);
        out += buf;
    }
    return out;
}

std::string e(const std::string& value) {
    std::string h = md5_hex(value);
    std::string result = h.substr(0, 3);
    std::string type_flag;
    std::vector<std::string> chunks;

    if (is_digit_string(value)) {
        type_flag = "3";
        size_t i = 0;
        while (i < value.size()) {
            std::string part = value.substr(i, std::min<size_t>(9, value.size() - i));
            // 转 64 位整数再 %x，避免大数问题（9 位十进制 < 2^30，安全）
            unsigned long long v = strtoull(part.c_str(), nullptr, 10);
            char buf[20];
            snprintf(buf, sizeof(buf), "%llx", v);
            chunks.push_back(std::string(buf));
            i += 9;
        }
    } else {
        type_flag = "4";
        chunks.push_back(byte_hex(value));
    }

    result += type_flag;
    result += "2" + h.substr(h.size() - 2);

    for (size_t i = 0; i < chunks.size(); i++) {
        char lenbuf[4];
        snprintf(lenbuf, sizeof(lenbuf), "%02x", (unsigned int)chunks[i].size());
        result += lenbuf;
        result += chunks[i];
        if (i + 1 < chunks.size()) result += "g";
    }

    while (result.size() < 20) {
        result += h.substr(0, 20 - result.size());
    }

    result += md5_hex(result).substr(0, 3);
    return result;
}

std::string sign(const std::string& query) {
    int64_t a = 0x15051505;
    int64_t b = a;
    int length = (int)query.size();
    int i = length; // 1-based index in Lua; we use i-1 / i-2 for 0-based

    while (i > 1) {
        unsigned char ci   = (unsigned char)query[i - 1];
        unsigned char ci_1 = (unsigned char)query[i - 2];
        a = (a ^ ((int64_t)ci   << ((length - i + 1) % 30))) & 0x7fffffff;
        b = (b ^ ((int64_t)ci_1 << ((i - 1) % 30))) & 0x7fffffff;
        i -= 2;
    }
    char buf[24];
    snprintf(buf, sizeof(buf), "%llx", (unsigned long long)(a + b));
    return to_lower(std::string(buf));
}

// 参数按 key 升序，拼 key=urlencode(value)，用 & 连接；跳过 key=="s"
std::string sorted_query(const std::vector<std::pair<std::string,std::string>>& params,
                         std::string& out_query_for_sign) {
    std::vector<std::pair<std::string,std::string>> sorted = params;
    std::sort(sorted.begin(), sorted.end(),
              [](const std::pair<std::string,std::string>& x,
                 const std::pair<std::string,std::string>& y){ return x.first < y.first; });
    std::string q;
    bool first = true;
    for (auto& kv : sorted) {
        if (kv.first == "s") continue;
        if (!first) q += "&";
        q += kv.first + "=" + url_encode(kv.second);
        first = false;
    }
    out_query_for_sign = q;
    return q;
}

std::vector<std::pair<std::string,std::string>> make_content_params(
    const std::string& book_id, const std::string& chapter_uid,
    const std::string& psvts, bool style, int sc) {
    long ct = (long)time(nullptr);
    if (e(std::to_string(ct)) == psvts) ct += 1;

    std::vector<std::pair<std::string,std::string>> p;
    p.push_back({"b", e(book_id)});
    p.push_back({"c", e(chapter_uid)});
    // r = random(0..9999)^2
    long rv = (long)(esp_random() % 10000);
    p.push_back({"r", std::to_string(rv * rv)});
    p.push_back({"ct", std::to_string(ct)});
    p.push_back({"ps", psvts});
    p.push_back({"pc", e(std::to_string(ct))});
    p.push_back({"sc", std::to_string(sc)});
    p.push_back({"prevChapter", "false"});
    p.push_back({"st", style ? "1" : "0"});

    std::string q;
    sorted_query(p, q);
    p.push_back({"s", sign(q)});
    return p;
}

std::string checked_body(const std::string& shard) {
    if (shard.size() <= 32) return "";
    std::string expected = shard.substr(0, 32);
    std::string body = shard.substr(32);
    std::string actual = to_upper(md5_hex(body));
    if (actual != expected) return "";
    return body;
}

// --- 字符交换还原 ---
static std::vector<int> swap_positions(const std::string& enc) {
    std::vector<int> result;
    int length = (int)enc.size();
    if (length < 4) return result;
    if (length < 11) { result.push_back(0); result.push_back(2); return result; }

    int n = std::min(4, (length + 9) / 10);
    std::string tmp;
    char numbuf[16];
    for (int i = length - 1; i >= length - n; i--) {
        // 原算法（weread.lua）：tonumber(bin(byte), 4)——二进制数字符串按四进制解读，
        // 等价 bit 展开：第 b 位为 1 累加 4^b = 2^(2b)
        uint8_t v = (uint8_t)enc[i];
        uint32_t val = 0;
        for (int b = 0; b < 8; b++)
            if ((v >> b) & 1) val += (1U << (2 * b));
        snprintf(numbuf, sizeof(numbuf), "%lu", (unsigned long)val);
        tmp += numbuf;
    }

    int m = length - n - 2;
    if (m <= 0) return result;
    int step = (int)std::to_string(m).size();
    int i = 0;
    while ((int)result.size() < 10 && i + step < (int)tmp.size()) {
        int v1 = (int)(strtoul(tmp.substr(i, step).c_str(), nullptr, 10) % m);
        int v2 = (int)(strtoul(tmp.substr(i + 1, step).c_str(), nullptr, 10) % m);
        result.push_back(v1);
        result.push_back(v2);
        i += step;
    }
    return result;
}

static std::string reverse_swaps(const std::string& enc, const std::vector<int>& pos) {
    std::string chars = enc;
    int plen = (int)pos.size();
    for (int i = plen - 1; i > 0; i -= 2) {
        for (int k = 1; k >= 0; k--) {
            int left = pos[i] + k;
            int right = pos[i - 1] + k;
            if (left >= 0 && right >= 0 && left < (int)chars.size() && right < (int)chars.size()) {
                char t = chars[left];
                chars[left] = chars[right];
                chars[right] = t;
            }
        }
    }
    return chars;
}

std::string decode_content_shards(const std::string& s0, const std::string& s1, const std::string& s2) {
    std::string payload = checked_body(s0) + checked_body(s1) + checked_body(s2);
    if (payload.empty()) return "";
    // 去首字符
    std::string enc = payload.substr(1);
    std::string reordered = reverse_swaps(enc, swap_positions(enc));

    // base64url → base64，滤掉非法字符
    std::string b64;
    b64.reserve(reordered.size());
    for (size_t i = 0; i < reordered.size(); i++) {
        char c = reordered[i];
        if (c == '-') b64 += '+';
        else if (c == '_') b64 += '/';
        else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '+' || c == '/')
            b64 += c;
    }
    while (b64.size() % 4) b64 += '=';

    size_t out_len = 0;
    // 先求解码后长度
    if (mbedtls_base64_decode(nullptr, 0, &out_len,
                              (const uint8_t*)b64.data(), b64.size()) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
        && out_len == 0)
        return "";
    uint8_t* buf = (uint8_t*)heap_caps_malloc(out_len + 1, MALLOC_CAP_SPIRAM);
    if (!buf) buf = (uint8_t*)malloc(out_len + 1);
    if (!buf) return "";
    size_t written = 0;
    if (mbedtls_base64_decode(buf, out_len, &written,
                              (const uint8_t*)b64.data(), b64.size()) != 0) {
        free(buf);
        return "";
    }
    buf[written] = 0;
    std::string result((char*)buf, written); // 已是 UTF-8
    free(buf);
    return result;
}

// --- PSRAM 版解码（大章节用） ---

// swap_positions 的指针版（逻辑与 std::string 版一致，避免复制大缓冲进 DRAM）
static std::vector<int> swap_positions_ptr(const char* enc, int length) {
    std::vector<int> result;
    if (length < 4) return result;
    if (length < 11) { result.push_back(0); result.push_back(2); return result; }

    int n = std::min(4, (length + 9) / 10);
    std::string tmp;
    char numbuf[16];
    for (int i = length - 1; i >= length - n; i--) {
        uint8_t v = (uint8_t)enc[i];
        uint32_t val = 0;
        for (int b = 0; b < 8; b++)
            if ((v >> b) & 1) val += (1U << (2 * b));
        snprintf(numbuf, sizeof(numbuf), "%lu", (unsigned long)val);
        tmp += numbuf;
    }

    int m = length - n - 2;
    if (m <= 0) return result;
    int step = (int)std::to_string(m).size();
    int i = 0;
    while ((int)result.size() < 10 && i + step < (int)tmp.size()) {
        int v1 = (int)(strtoul(tmp.substr(i, step).c_str(), nullptr, 10) % m);
        int v2 = (int)(strtoul(tmp.substr(i + 1, step).c_str(), nullptr, 10) % m);
        result.push_back(v1);
        result.push_back(v2);
        i += step;
    }
    return result;
}

char* decode_content_shards_psram(const char* s0, size_t n0,
                                  const char* s1, size_t n1,
                                  const char* s2, size_t n2, size_t* out_len) {
    // 1. PSRAM 拼接三个分片 body（各去 32 字节 MD5 前缀并校验）
    size_t cap = n0 + n1 + n2;
    if (!cap) return nullptr;
    char* buf = (char*)heap_caps_malloc(cap + 1, MALLOC_CAP_SPIRAM);
    if (!buf) return nullptr;
    size_t n = 0;
    const char* shards[3] = {s0, s1, s2};
    size_t lens[3] = {n0, n1, n2};
    for (int k = 0; k < 3; k++) {
        const char* shard = shards[k];
        size_t slen = lens[k];
        if (!shard || slen <= 32) continue;
        // MD5 校验：body 的 md5(大写) 须等于前 32 字节；任一不等丢弃该片
        std::string actual = to_upper(md5_hex((const uint8_t*)shard + 32, slen - 32));
        if (actual.size() != 32 || memcmp(shard, actual.c_str(), 32) != 0) continue;
        memcpy(buf + n, shard + 32, slen - 32);
        n += slen - 32;
    }
    if (!n) { heap_caps_free(buf); return nullptr; }

    // 2. 去首字符（指针偏移，不复制）
    char* enc = buf + 1;
    int enc_len = (int)n - 1;

    // 3. 字符交换还原（原地交换，与 reverse_swaps 语义等价）
    std::vector<int> pos = swap_positions_ptr(enc, enc_len);
    int plen = (int)pos.size();
    for (int i = plen - 1; i > 0; i -= 2) {
        for (int k = 1; k >= 0; k--) {
            int left = pos[i] + k;
            int right = pos[i - 1] + k;
            if (left >= 0 && right >= 0 && left < enc_len && right < enc_len) {
                char t = enc[left];
                enc[left] = enc[right];
                enc[right] = t;
            }
        }
    }

    // 4. base64url → base64，非法字符原地过滤压缩
    int m = 0;
    for (int i = 0; i < enc_len; i++) {
        char c = enc[i];
        if (c == '-') c = '+';
        else if (c == '_') c = '/';
        else if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                   (c >= '0' && c <= '9') || c == '+' || c == '/')) continue;
        enc[m++] = c;
    }
    while (m % 4) enc[m++] = '=';

    // 5. base64 解码到第二个 PSRAM 缓冲（输出缓冲）
    size_t olen = 0;
    if (mbedtls_base64_decode(nullptr, 0, &olen, (const uint8_t*)enc, m) != MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL
        && olen == 0) {
        heap_caps_free(buf);
        return nullptr;
    }
    char* out = (char*)heap_caps_malloc(olen + 1, MALLOC_CAP_SPIRAM);
    if (!out) { heap_caps_free(buf); return nullptr; }
    size_t written = 0;
    if (mbedtls_base64_decode((uint8_t*)out, olen, &written, (const uint8_t*)enc, m) != 0) {
        heap_caps_free(out);
        heap_caps_free(buf);
        return nullptr;
    }
    heap_caps_free(buf);
    out[written] = 0; // 补结尾便于字符串扫描
    *out_len = written;
    return out;
}

} // namespace weread
