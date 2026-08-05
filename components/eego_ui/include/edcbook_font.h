// edcbook_font.h — EDCBook .bin 字体解析与渲染（纯 ESP-IDF 移植版，Task 7）
//
// 格式（由 EDCBook_FontTool_1.1.py 逆向得出）：
//   [头]    u32 char_count + u8 font_height              （5 字节）
//   [条目×N] u16 unicode, u16 advance, u8 w, u8 h,
//           i8 xo, i8 yo, u32 bmp_off, u32 bmp_size, u32 cached  （20 字节/条）
//   [位图]  霍夫曼位流(MSB first):
//             "0"       → 白(15)
//             "10"      → 黑(0)
//             "11"+4bit → 灰(1..14)
//
// 索引与字形缓存均放 PSRAM（缓存上限 1536KB，超限整体清空）。
// 文件句柄常驻以支持位图随机读取（SdCard::instance().open 返回 FILE*）。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include <map>
#include <vector>

#include "portrait_canvas.h"

namespace eego {

class EdcFont {
public:
    EdcFont() = default;
    ~EdcFont();

    EdcFont(const EdcFont&) = delete;
    EdcFont& operator=(const EdcFont&) = delete;

    // 从 SD 加载 .bin（索引进 PSRAM）。重载会先释放旧索引与旧缓存。
    bool load(const char* path);
    void unload();
    bool loaded() const { return loaded_; }
    int  fontHeight() const { return font_height_; }

    // 二分查找字符条目；返回索引或 -1（条目按 unicode 升序）
    int findGlyph(uint16_t cp) const;

    // 解码一个字符的灰度位图到 out（w*h 字节，0=黑..15=白，行优先）。
    bool decodeGlyph(int idx, std::vector<uint8_t>& out,
                     int& w, int& h, int& xo, int& yo, int& adv);

    // 测量一行 UTF-8 文本的像素宽度
    int textWidth(const char* utf8);

    // 在 (x, y) 绘制 UTF-8 文本：y 为字形 cell 顶部，scale 为整数放大，
    // bold 走 x+1 二次绘制仿粗。返回结束 x 坐标。
    int drawText(PortraitCanvas& canvas, const char* utf8,
                 int x, int y, int scale = 1, bool bold = false);

    // 以统一灰度 gray(0-15, 0=黑/15=白)绘制文本（所有墨色像素渲染为 gray）。
    // 用于设置页等需要"深灰辅助文字"的场景。返回结束 x 坐标。
    int drawTextGray(PortraitCanvas& canvas, const char* utf8,
                     int x, int y, int scale, uint8_t gray);

    // UTF-8 解码下一个码点（推进 p）
    static uint32_t nextCodepoint(const uint8_t*& p);

private:
    struct Entry {
        uint16_t cp;
        uint16_t adv;
        uint8_t  w, h;
        int8_t   xo, yo;
        uint32_t off, size;
    };
    // 字形解码缓存（pix 在 PSRAM）
    struct GCEnt {
        int w, h, xo, yo, adv;
        uint8_t* pix;
    };

    std::map<uint16_t, GCEnt> cache_;
    size_t cache_bytes_ = 0;
    static const size_t CACHE_CAP = 1536 * 1024;

    bool      loaded_ = false;
    int       font_height_ = 0;
    uint32_t  char_count_ = 0;
    Entry*    entries_ = nullptr;   // PSRAM（回退内部 RAM）
    FILE*     file_ = nullptr;       // 常驻句柄读位图
    char      path_[160] = {};

    bool readAt(uint32_t off, uint8_t* buf, size_t n);
    int  drawOnce(PortraitCanvas& canvas, const char* utf8,
                  int x, int y, int scale, uint8_t gray);
};

}  // namespace eego
