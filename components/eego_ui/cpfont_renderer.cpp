// cpfont_renderer.cpp — CPFONT v4 / .eefont 字体解析实现（纯 ESP-IDF 移植版，Task 8）
//
// 移植自 eego-a4-template-ref/lib/EegoA4Ui/src/CpFontRenderer.cpp，关键改动：
//   - SDCardManager + SdFat FsFile → SdCard::instance() + FILE*（fseek/fread）
//   - magic 校验放宽：同时接受 "CPFONT\0\0" 与 "EEFONT\0\0"（.eefont 换皮核心）
//   - PortraitCanvas 由 drawText 参数传入（不持有引用）
//   - 增加 scale / bold 渲染、textWidth、fontHeight
//   - Arduino Serial → ESP_LOGx
#include "cpfont_renderer.h"

#include <algorithm>
#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sd_card.h"

namespace eego {

static const char* TAG = "eego.cpfont";

constexpr uint16_t CPFONT_VERSION = 4;
constexpr uint32_t GLOBAL_HEADER_BYTES = 32;
constexpr uint32_t STYLE_TOC_BYTES = 32;
constexpr uint32_t INTERVAL_BYTES = 12;
constexpr uint32_t GLYPH_BYTES = 16;
constexpr uint32_t KERN_ENTRY_BYTES = 3;
constexpr uint32_t LIGATURE_BYTES = 8;
constexpr uint32_t MAX_INTERVALS = 4096;
constexpr uint32_t MAX_GLYPHS = 65536;

// PSRAM 优先，失败回退内部 RAM
static void* allocPsram(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

// 同时接受 "CPFONT\0\0" 与 "EEFONT\0\0"
static bool isValidMagic(const uint8_t* header) {
    if (header[6] != 0 || header[7] != 0) return false;
    if (memcmp(header, "CPFONT", 6) == 0) return true;
    if (memcmp(header, "EEFONT", 6) == 0) return true;
    return false;
}

CpFontRenderer::~CpFontRenderer() {
    unload();
    if (bitmap_) heap_caps_free(bitmap_);
}

uint16_t CpFontRenderer::readU16(const uint8_t* data) {
    return (uint16_t)data[0] | ((uint16_t)data[1] << 8);
}

int16_t CpFontRenderer::readI16(const uint8_t* data) {
    return (int16_t)readU16(data);
}

uint32_t CpFontRenderer::readU32(const uint8_t* data) {
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

bool CpFontRenderer::readAt(const uint32_t offset, void* destination, const size_t bytes) {
    if (!file_) return false;
    if (fseek(file_, (long)offset, SEEK_SET) != 0) return false;
    return fread(destination, 1, bytes, file_) == bytes;
}

void CpFontRenderer::unload() {
    loaded_ = false;
    if (file_) { fclose(file_); file_ = nullptr; }
    if (intervals_) { heap_caps_free(intervals_); intervals_ = nullptr; }
    info_ = {};
    glyphsFileOffset_ = 0;
    bitmapFileOffset_ = 0;
}

bool CpFontRenderer::load(const char* path) {
    unload();
    if (!path) return false;

    if (!SdCard::instance().ready()) {
        ESP_LOGE(TAG, "SD 未就绪");
        return false;
    }
    file_ = SdCard::instance().open(path, "rb");
    if (!file_) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return false;
    }

    // 文件大小
    if (fseek(file_, 0, SEEK_END) != 0) { unload(); return false; }
    long fsz = ftell(file_);
    if (fsz < 0) { unload(); return false; }
    info_.fileBytes = (uint32_t)fsz;
    rewind(file_);

    uint8_t header[GLOBAL_HEADER_BYTES] = {};
    if (!readAt(0, header, sizeof(header))) {
        ESP_LOGE(TAG, "全局头读取失败");
        unload();
        return false;
    }
    if (!isValidMagic(header)) {
        ESP_LOGE(TAG, "无效 magic（非 CPFONT/EEFONT）");
        unload();
        return false;
    }
    if (readU16(header + 8) != CPFONT_VERSION) {
        ESP_LOGE(TAG, "不支持的版本: %u", (unsigned)readU16(header + 8));
        unload();
        return false;
    }
    info_.is2Bit = (readU16(header + 10) & 1U) != 0;
    const uint8_t styleCount = header[12];
    if (styleCount == 0 || styleCount > 4) {
        ESP_LOGE(TAG, "无效 styleCount: %u", styleCount);
        unload();
        return false;
    }

    uint8_t chosen[STYLE_TOC_BYTES] = {};
    bool haveStyle = false;
    for (uint8_t index = 0; index < styleCount; ++index) {
        uint8_t toc[STYLE_TOC_BYTES] = {};
        if (!readAt(GLOBAL_HEADER_BYTES + index * STYLE_TOC_BYTES, toc, sizeof(toc))) {
            ESP_LOGE(TAG, "样式表读取失败");
            unload();
            return false;
        }
        if (!haveStyle || toc[0] == 0) {
            memcpy(chosen, toc, sizeof(chosen));
            haveStyle = true;
        }
        if (toc[0] == 0) break;
    }

    info_.styleId = chosen[0];
    info_.intervalCount = readU32(chosen + 4);
    info_.glyphCount = readU32(chosen + 8);
    info_.advanceY = chosen[12];
    info_.ascender = readI16(chosen + 13);
    info_.descender = readI16(chosen + 15);
    const uint16_t kernLeftEntries = readU16(chosen + 17);
    const uint16_t kernRightEntries = readU16(chosen + 19);
    const uint8_t kernLeftClasses = chosen[21];
    const uint8_t kernRightClasses = chosen[22];
    const uint8_t ligatureCount = chosen[23];
    const uint32_t dataOffset = readU32(chosen + 24);

    if (info_.intervalCount == 0 || info_.intervalCount > MAX_INTERVALS ||
        info_.glyphCount == 0 || info_.glyphCount > MAX_GLYPHS) {
        ESP_LOGE(TAG, "区间/字形数异常: intervals=%lu glyphs=%lu",
                 (unsigned long)info_.intervalCount, (unsigned long)info_.glyphCount);
        unload();
        return false;
    }

    const uint64_t glyphsOffset =
        (uint64_t)dataOffset + (uint64_t)info_.intervalCount * INTERVAL_BYTES;
    const uint64_t bitmapOffset =
        glyphsOffset + (uint64_t)info_.glyphCount * GLYPH_BYTES +
        (uint64_t)(kernLeftEntries + kernRightEntries) * KERN_ENTRY_BYTES +
        (uint64_t)kernLeftClasses * kernRightClasses +
        (uint64_t)ligatureCount * LIGATURE_BYTES;
    if (glyphsOffset > UINT32_MAX || bitmapOffset > info_.fileBytes) {
        ESP_LOGE(TAG, "段偏移越界");
        unload();
        return false;
    }
    glyphsFileOffset_ = (uint32_t)glyphsOffset;
    bitmapFileOffset_ = (uint32_t)bitmapOffset;

    const size_t intervalBytes = (size_t)info_.intervalCount * sizeof(Interval);
    intervals_ = (Interval*)allocPsram(intervalBytes);
    if (!intervals_) {
        ESP_LOGE(TAG, "区间表内存分配失败");
        unload();
        return false;
    }
    if (!readAt(dataOffset, intervals_, intervalBytes)) {
        ESP_LOGE(TAG, "区间表读取失败");
        unload();
        return false;
    }

    // 校验区间表覆盖所有字形
    uint32_t expectedGlyphOffset = 0;
    uint32_t previousLast = 0;
    for (uint32_t index = 0; index < info_.intervalCount; ++index) {
        const Interval& interval = intervals_[index];
        if (interval.first > interval.last ||
            (index && interval.first <= previousLast) ||
            interval.offset != expectedGlyphOffset) {
            ESP_LOGE(TAG, "无效区间: %lu", (unsigned long)index);
            unload();
            return false;
        }
        const uint64_t span = (uint64_t)interval.last - interval.first + 1;
        if (expectedGlyphOffset + span > info_.glyphCount) {
            ESP_LOGE(TAG, "区间越界字形表: %lu", (unsigned long)index);
            unload();
            return false;
        }
        expectedGlyphOffset += (uint32_t)span;
        previousLast = interval.last;
    }
    if (expectedGlyphOffset != info_.glyphCount) {
        ESP_LOGE(TAG, "区间表未覆盖所有字形");
        unload();
        return false;
    }

    loaded_ = true;
    ESP_LOGI(TAG, "loaded=%s bytes=%lu style=%u intervals=%lu glyphs=%lu "
                  "advanceY=%u asc=%d desc=%d depth=%u",
             path, (unsigned long)info_.fileBytes, info_.styleId,
             (unsigned long)info_.intervalCount, (unsigned long)info_.glyphCount,
             info_.advanceY, info_.ascender, info_.descender, info_.is2Bit ? 2 : 1);
    return true;
}

int32_t CpFontRenderer::findGlyphIndex(const uint32_t codepoint) const {
    if (!loaded_ || !intervals_) return -1;
    int32_t left = 0;
    int32_t right = (int32_t)info_.intervalCount - 1;
    while (left <= right) {
        const int32_t middle = left + (right - left) / 2;
        const Interval& interval = intervals_[middle];
        if (codepoint < interval.first) {
            right = middle - 1;
        } else if (codepoint > interval.last) {
            left = middle + 1;
        } else {
            return (int32_t)(interval.offset + codepoint - interval.first);
        }
    }
    return -1;
}

bool CpFontRenderer::hasCodepoint(const uint32_t codepoint) const {
    return findGlyphIndex(codepoint) >= 0;
}

bool CpFontRenderer::nextCodepoint(const char*& cursor, uint32_t& codepoint) {
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(cursor);
    if (!*bytes) return false;

    if (bytes[0] < 0x80) {
        codepoint = bytes[0];
        cursor += 1;
        return true;
    }

    uint8_t length = 0;
    uint32_t value = 0;
    uint32_t minimum = 0;
    if ((bytes[0] & 0xe0U) == 0xc0U) {
        length = 2; value = bytes[0] & 0x1fU; minimum = 0x80;
    } else if ((bytes[0] & 0xf0U) == 0xe0U) {
        length = 3; value = bytes[0] & 0x0fU; minimum = 0x800;
    } else if ((bytes[0] & 0xf8U) == 0xf0U) {
        length = 4; value = bytes[0] & 0x07U; minimum = 0x10000;
    } else {
        codepoint = 0xfffd;
        cursor += 1;
        return true;
    }

    for (uint8_t index = 1; index < length; ++index) {
        if ((bytes[index] & 0xc0U) != 0x80U) {
            codepoint = 0xfffd;
            cursor += 1;
            return true;
        }
        value = (value << 6) | (bytes[index] & 0x3fU);
    }
    if (value < minimum || value > 0x10ffff ||
        (value >= 0xd800 && value <= 0xdfff)) {
        codepoint = 0xfffd;
        cursor += 1;
        return true;
    }
    codepoint = value;
    cursor += length;
    return true;
}

uint16_t CpFontRenderer::countMissing(const char* utf8Text) const {
    if (!loaded_ || !utf8Text) return 0xffff;
    uint16_t missing = 0;
    const char* cursor = utf8Text;
    uint32_t codepoint = 0;
    while (nextCodepoint(cursor, codepoint)) {
        if (!hasCodepoint(codepoint)) ++missing;
    }
    return missing;
}

bool CpFontRenderer::readGlyph(const uint32_t glyphIndex, Glyph& glyph) {
    if (glyphIndex >= info_.glyphCount) return false;
    uint8_t raw[GLYPH_BYTES] = {};
    if (!readAt(glyphsFileOffset_ + glyphIndex * GLYPH_BYTES, raw, sizeof(raw))) {
        return false;
    }
    glyph.width = raw[0];
    glyph.height = raw[1];
    glyph.advanceX = readU16(raw + 2);
    glyph.left = readI16(raw + 4);
    glyph.top = readI16(raw + 6);
    glyph.dataLength = readU16(raw + 8);
    glyph.dataOffset = readU32(raw + 12);
    return (uint64_t)bitmapFileOffset_ + glyph.dataOffset + glyph.dataLength <= info_.fileBytes;
}

bool CpFontRenderer::ensureBitmapCapacity(const size_t bytes) {
    if (bytes <= bitmapCapacity_) return true;
    size_t capacity = std::max<size_t>(bytes, bitmapCapacity_ * 2);
    if (capacity < 256) capacity = 256;
    auto* replacement = (uint8_t*)allocPsram(capacity);
    if (!replacement) return false;
    if (bitmap_) heap_caps_free(bitmap_);
    bitmap_ = replacement;
    bitmapCapacity_ = capacity;
    return true;
}

void CpFontRenderer::drawGlyph(PortraitCanvas& canvas, const int16_t cursorX,
                               const int16_t baseline, const Glyph& glyph,
                               const int16_t scale, const uint8_t gray) {
    const int16_t originX = cursorX + glyph.left * scale;
    const int16_t originY = baseline - glyph.top * scale;
    const uint32_t pixels = (uint32_t)glyph.width * glyph.height;
    // 电子墨水屏只有黑白两色，2-bit 灰度抗锯齿经 Bayer 抖动后边缘会产生白点/噪点。
    // 直接二值化：level >= 1 即画纯黑，既消除白点又保持笔画清晰。

    for (uint32_t position = 0; position < pixels; ++position) {
        const int16_t gx = (int16_t)(position % glyph.width);
        const int16_t gy = (int16_t)(position / glyph.width);
        const int16_t px = originX + gx * scale;
        const int16_t py = originY + gy * scale;
        bool ink = false;
        if (info_.is2Bit) {
            const uint8_t packed = bitmap_[position >> 2];
            const uint8_t shift = (uint8_t)((3 - (position & 3U)) * 2);
            uint8_t level = (packed >> shift) & 0x03U;
            if (level == 0) continue;  // 透明/白
            ink = true;
        } else {
            const uint8_t packed = bitmap_[position >> 3];
            ink = ((packed >> (7 - (position & 7U))) & 0x01U) != 0;
        }
        if (!ink) continue;
        // gray==0 走纯黑（drawPixel）；gray>0 走 drawPixelGray 统一灰度填充
        if (gray == 0) {
            if (scale == 1) {
                canvas.drawPixel(px, py, 0);
            } else {
                for (int16_t sy = 0; sy < scale; sy++)
                    for (int16_t sx = 0; sx < scale; sx++)
                        canvas.drawPixel(px + sx, py + sy, 0);
            }
        } else {
            if (scale == 1) {
                canvas.drawPixelGray(px, py, gray);
            } else {
                for (int16_t sy = 0; sy < scale; sy++)
                    for (int16_t sx = 0; sx < scale; sx++)
                        canvas.drawPixelGray(px + sx, py + sy, gray);
            }
        }
    }
}

int16_t CpFontRenderer::drawOnce(PortraitCanvas& canvas, const int16_t x,
                                 const int16_t top, const char* utf8Text,
                                 const int16_t scale, const uint8_t gray) {
    if (!loaded_ || !utf8Text) return x;
    int16_t cursorX = x;
    const int16_t baseline = top + (int16_t)info_.ascender * scale;
    const char* cursor = utf8Text;
    uint32_t codepoint = 0;
    while (nextCodepoint(cursor, codepoint)) {
        const int32_t glyphIndex = findGlyphIndex(codepoint);
        if (glyphIndex < 0) {
            cursorX += (int16_t)(info_.ascender / 2) * scale;
            continue;
        }
        Glyph glyph;
        if (!readGlyph((uint32_t)glyphIndex, glyph) ||
            !ensureBitmapCapacity(glyph.dataLength) ||
            (glyph.dataLength &&
             !readAt(bitmapFileOffset_ + glyph.dataOffset, bitmap_, glyph.dataLength))) {
            continue;
        }
        if (glyph.dataLength) drawGlyph(canvas, cursorX, baseline, glyph, scale, gray);
        const int16_t eff = (int16_t)(((uint32_t)glyph.advanceX + 8U) >> 4);
        cursorX = (int16_t)(cursorX + eff * scale);
    }
    return cursorX;
}

int16_t CpFontRenderer::drawText(PortraitCanvas& canvas, const int16_t x,
                                 const int16_t top, const char* utf8Text,
                                 int16_t scale, bool bold) {
    if (scale < 1) scale = 1;
    int16_t endX = drawOnce(canvas, x, top, utf8Text, scale, 0);
    if (bold) drawOnce(canvas, (int16_t)(x + 1), top, utf8Text, scale, 0);
    return endX;
}

int16_t CpFontRenderer::drawTextGray(PortraitCanvas& canvas, const int16_t x,
                                     const int16_t top, const char* utf8Text,
                                     int16_t scale, uint8_t gray) {
    if (scale < 1) scale = 1;
    if (gray > 15) gray = 15;
    return drawOnce(canvas, x, top, utf8Text, scale, gray);
}

int16_t CpFontRenderer::textWidth(const char* utf8Text) {
    if (!loaded_ || !utf8Text) return 0;
    int16_t width = 0;
    const char* cursor = utf8Text;
    uint32_t codepoint = 0;
    Glyph glyph;
    while (nextCodepoint(cursor, codepoint)) {
        const int32_t gi = findGlyphIndex(codepoint);
        if (gi < 0) {
            width += (int16_t)(info_.ascender / 2);
            continue;
        }
        if (!readGlyph((uint32_t)gi, glyph)) {
            width += (int16_t)(info_.ascender / 2);
            continue;
        }
        width += (int16_t)(((uint32_t)glyph.advanceX + 8U) >> 4);
    }
    return width;
}

int16_t CpFontRenderer::fontHeight() const {
    if (info_.advanceY > 0) return (int16_t)info_.advanceY;
    return (int16_t)(info_.ascender - info_.descender);
}

}  // namespace eego
