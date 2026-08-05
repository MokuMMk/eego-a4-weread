// cpfont_renderer.h — CPFONT v4 / .eefont 字体解析与渲染（纯 ESP-IDF 移植版，Task 8）
//
// 格式（CPFONT v4，同时接受 .eefont 换皮）：
//   [全局头 32B]  magic 8B + u16 version + u16 flags + u8 styleCount + ...
//   [样式 TOC 32B × styleCount]
//   [区间表 12B × intervalCount]   first/u32, last/u32, offset/u32
//   [字形表 16B × glyphCount]      w,h,advanceX(u16),left(i16),top(i16),
//                                  dataLen(u16),dataOff(u32)
//   [kern / ligature ...]
//   [位图区] 1-bit(8px/byte,MSB) 或 2-bit(4px/byte,4 级灰度)
//
// magic 校验放宽：同时接受 "CPFONT\0\0" 与 "EEFONT\0\0"（.eefont 支持）。
// advanceX 为 1/16px 单位，有效像素 advance = (advanceX + 8) >> 4。
// 渲染：drawText(x, top, utf8)，baseline = top + ascender。
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>

#include "portrait_canvas.h"

namespace eego {

class CpFontRenderer {
public:
    struct FontInfo {
        uint32_t fileBytes = 0;
        uint32_t intervalCount = 0;
        uint32_t glyphCount = 0;
        uint8_t advanceY = 0;
        int16_t ascender = 0;
        int16_t descender = 0;
        bool is2Bit = false;
        uint8_t styleId = 0;
    };

    CpFontRenderer() = default;
    ~CpFontRenderer();

    CpFontRenderer(const CpFontRenderer&) = delete;
    CpFontRenderer& operator=(const CpFontRenderer&) = delete;

    bool load(const char* path);
    void unload();
    bool loaded() const { return loaded_; }
    const FontInfo& info() const { return info_; }

    bool hasCodepoint(uint32_t codepoint) const;
    uint16_t countMissing(const char* utf8Text) const;

    // 在 (x, top) 绘制 UTF-8 文本：baseline = top + ascender。
    // scale 整数放大，bold 走 x+1 二次绘制仿粗。返回结束 x 坐标。
    int16_t drawText(PortraitCanvas& canvas, int16_t x, int16_t top,
                     const char* utf8Text, int16_t scale = 1, bool bold = false);

    // 以统一灰度 gray(0-15, 0=黑/15=白)绘制文本（用 drawPixelGray 上屏）。
    // 用于设置页等需要"深灰辅助文字"的场景。返回结束 x 坐标。
    int16_t drawTextGray(PortraitCanvas& canvas, int16_t x, int16_t top,
                         const char* utf8Text, int16_t scale, uint8_t gray);

    // 测量一行 UTF-8 文本的像素宽度（按 effective advance 累加）
    int16_t textWidth(const char* utf8Text);

    // 字体高度（行高：优先 advanceY，否则 ascender - descender）
    int16_t fontHeight() const;

private:
    struct Interval {
        uint32_t first;
        uint32_t last;
        uint32_t offset;
    };
    struct Glyph {
        uint8_t width = 0;
        uint8_t height = 0;
        uint16_t advanceX = 0;
        int16_t left = 0;
        int16_t top = 0;
        uint16_t dataLength = 0;
        uint32_t dataOffset = 0;
    };

    FontInfo info_{};
    Interval* intervals_ = nullptr;   // PSRAM（回退内部 RAM）
    uint8_t* bitmap_ = nullptr;       // 位图读取缓冲（PSRAM 优先）
    size_t bitmapCapacity_ = 0;
    FILE* file_ = nullptr;
    uint32_t glyphsFileOffset_ = 0;
    uint32_t bitmapFileOffset_ = 0;
    bool loaded_ = false;

    static uint16_t readU16(const uint8_t* data);
    static int16_t readI16(const uint8_t* data);
    static uint32_t readU32(const uint8_t* data);
    static bool nextCodepoint(const char*& cursor, uint32_t& codepoint);

    bool readAt(uint32_t offset, void* destination, size_t bytes);
    int32_t findGlyphIndex(uint32_t codepoint) const;
    bool readGlyph(uint32_t glyphIndex, Glyph& glyph);
    bool ensureBitmapCapacity(size_t bytes);
    void drawGlyph(PortraitCanvas& canvas, int16_t cursorX, int16_t baseline,
                   const Glyph& glyph, int16_t scale, uint8_t gray);
    int16_t drawOnce(PortraitCanvas& canvas, int16_t x, int16_t top,
                     const char* utf8Text, int16_t scale, uint8_t gray);
};

}  // namespace eego
