// weread_crypto.h — 微信读书 Web 协议加密/解码三件套
// 移植自 finlater/weread.koplugin 的 lib/weread.lua 与 scripts/fetch_weread_epub.py
// 纯 ESP-IDF：std::string 替代 Arduino String，mbedtls 提供 MD5/base64。
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>
#include <utility>

namespace weread {

// MD5 hex（小写 32 字符）
std::string md5_hex(const uint8_t* data, size_t len);
inline std::string md5_hex(const std::string& s) {
    return md5_hex(reinterpret_cast<const uint8_t*>(s.data()), s.size());
}

// URL 编码（对应 JS encodeURIComponent：保留 A-Z a-z 0-9 - _ . ~）
std::string url_encode(const std::string& s);

// _e(value) 哈希：对 bookId/chapterUid/时间戳的确定性编码
//   数字串 → type '3'，按 9 位分段转 hex
//   非数字 → type '4'，逐字节 hex
std::string e(const std::string& value);
inline std::string e(long v) { return e(std::to_string(v)); }

// 签名 s：对按 key 排序的 query 串做自定义 hash（初始 0x15051505）
std::string sign(const std::string& query);

// 把参数表按 key 排序拼成 query 串（不含 s），并返回；调用方随后 sign 之
std::string sorted_query(const std::vector<std::pair<std::string,std::string>>& params,
                         std::string& out_query_for_sign);

// 构造正文分片请求参数（b/c/r/ct/ps/pc/sc/prevChapter/st/s）
// psvts 来自 reader HTML；style=true 用于 e_2（CSS）
// 返回可直接 JSON 序列化的 key=value 列表（已含 s）
std::vector<std::pair<std::string,std::string>> make_content_params(
    const std::string& book_id, const std::string& chapter_uid,
    const std::string& psvts, bool style = false, int sc = 1);

// ---- 响应解码 ----
// 校验并去掉 32 位 MD5 前缀；失败返回空串
std::string checked_body(const std::string& shard);

// 正文解码：拼接 e0+e1+e3（或 t0+t1）的 body，去首字符、字符交换还原、base64url 解码
// 返回 UTF-8 明文字符串
std::string decode_content_shards(const std::string& s0, const std::string& s1,
                                  const std::string& s2);

// PSRAM 版：算法同上，但中间过程与结果全部在 PSRAM 缓冲进行，不受 String 64KB 限制
// 输入为三个分片的原始数据（指针+长度，可在 PSRAM；nullptr/长度≤32 的片跳过）。
// 成功返回 PSRAM 明文缓冲（heap_caps_malloc，调用方负责 heap_caps_free），
// *out_len 为明文长度（不含结尾 \0，但缓冲已补 \0 便于字符串扫描）；失败返回 nullptr
char* decode_content_shards_psram(const char* s0, size_t n0,
                                  const char* s1, size_t n1,
                                  const char* s2, size_t n2, size_t* out_len);

} // namespace weread
