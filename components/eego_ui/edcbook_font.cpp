// edcbook_font.cpp — EDCBook .bin 字体解析实现（纯 ESP-IDF 移植版，Task 7）
//
// 移植自 papers3-weread-ref/src/edcbook_font.cpp，关键改动：
//   - Arduino SD/File → SdCard::instance().open() 返回 FILE*（fseek/fread）
//   - heap_caps 走 esp_heap_caps.h（PSRAM 优先，内部 RAM 回退）
//   - Arduino Serial → ESP_LOGx
//   - std::vector<Entry> → PSRAM 数组 Entry* entries_
//   - drawTextFontScaled 渲染逻辑内联到 EdcFont::drawText
#include "edcbook_font.h"

#include <cstring>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "sd_card.h"

namespace eego {

static const char* TAG = "eego.edcfont";

// 条目在文件中的固定大小（struct.pack('<HHBBbbIII') = 2+2+1+1+1+1+4+4+4 = 20）
static const int ENTRY_SIZE  = 20;
static const int HEADER_SIZE = 5;  // u32 char_count + u8 font_height

// PSRAM 优先，失败回退内部 RAM（保证无 PSRAM 时仍可加载小字体）
static void* allocPsram(size_t n) {
    void* p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM);
    if (!p) p = heap_caps_malloc(n, MALLOC_CAP_8BIT);
    return p;
}

uint32_t EdcFont::nextCodepoint(const uint8_t*& p) {
    uint32_t c = *p++;
    if (c < 0x80) return c;
    if ((c & 0xE0) == 0xC0) { c &= 0x1F; c = (c << 6) | (*p++ & 0x3F); return c; }
    if ((c & 0xF0) == 0xE0) {
        c &= 0x0F;
        c = (c << 6) | (*p++ & 0x3F);
        c = (c << 6) | (*p++ & 0x3F);
        return c;
    }
    if ((c & 0xF8) == 0xF0) {
        c &= 0x07;
        c = (c << 6) | (*p++ & 0x3F);
        c = (c << 6) | (*p++ & 0x3F);
        c = (c << 6) | (*p++ & 0x3F);
        return c;
    }
    return c;
}

bool EdcFont::readAt(uint32_t off, uint8_t* buf, size_t n) {
    if (!file_) return false;
    if (fseek(file_, (long)off, SEEK_SET) != 0) return false;
    return fread(buf, 1, n, file_) == n;
}

EdcFont::~EdcFont() {
    unload();
}

void EdcFont::unload() {
    loaded_ = false;
    if (file_) { fclose(file_); file_ = nullptr; }
    if (entries_) { heap_caps_free(entries_); entries_ = nullptr; }
    for (auto& kv : cache_) heap_caps_free(kv.second.pix);
    cache_.clear();
    cache_bytes_ = 0;
    char_count_ = 0;
    font_height_ = 0;
    path_[0] = '\0';
}

bool EdcFont::load(const char* path) {
    unload();
    if (!path) return false;
    strncpy(path_, path, sizeof(path_) - 1);
    path_[sizeof(path_) - 1] = '\0';

    if (!SdCard::instance().ready()) {
        ESP_LOGE(TAG, "SD 未就绪");
        return false;
    }
    file_ = SdCard::instance().open(path, "rb");
    if (!file_) {
        ESP_LOGE(TAG, "打开失败: %s", path);
        return false;
    }

    uint8_t hdr[HEADER_SIZE];
    if (fread(hdr, 1, HEADER_SIZE, file_) != HEADER_SIZE) {
        ESP_LOGE(TAG, "头读取失败");
        unload();
        return false;
    }
    char_count_ = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    font_height_ = hdr[4];
    if (char_count_ == 0 || char_count_ > 200000) {
        ESP_LOGE(TAG, "字符数异常: %u", (unsigned)char_count_);
        unload();
        return false;
    }
    ESP_LOGI(TAG, "%s 字符数=%u 字高=%d", path, (unsigned)char_count_, font_height_);

    // 条目索引读进 PSRAM
    size_t table_bytes = (size_t)char_count_ * ENTRY_SIZE;
    uint8_t* raw = (uint8_t*)allocPsram(table_bytes);
    if (!raw) {
        ESP_LOGE(TAG, "索引内存分配失败 (%u 字节)", (unsigned)table_bytes);
        unload();
        return false;
    }
    if (fread(raw, 1, table_bytes, file_) != table_bytes) {
        ESP_LOGE(TAG, "条目表读取失败");
        heap_caps_free(raw);
        unload();
        return false;
    }
    entries_ = (Entry*)allocPsram((size_t)char_count_ * sizeof(Entry));
    if (!entries_) {
        ESP_LOGE(TAG, "Entry 数组分配失败");
        heap_caps_free(raw);
        unload();
        return false;
    }
    for (uint32_t i = 0; i < char_count_; i++) {
        const uint8_t* e = raw + i * ENTRY_SIZE;
        Entry& en = entries_[i];
        en.cp   = e[0] | (e[1] << 8);
        en.adv  = e[2] | (e[3] << 8);
        en.w    = e[4];
        en.h    = e[5];
        en.xo   = (int8_t)e[6];
        en.yo   = (int8_t)e[7];
        en.off  = e[8] | (e[9] << 8) | (e[10] << 16) | ((uint32_t)e[11] << 24);
        en.size = e[12] | (e[13] << 8) | (e[14] << 16) | ((uint32_t)e[15] << 24);
    }
    heap_caps_free(raw);

    loaded_ = true;
    ESP_LOGI(TAG, "索引加载完成 %u 条", (unsigned)char_count_);
    return true;
}

int EdcFont::findGlyph(uint16_t cp) const {
    if (!entries_) return -1;
    int lo = 0, hi = (int)char_count_ - 1;
    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        if (entries_[mid].cp == cp) return mid;
        if (entries_[mid].cp < cp) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

bool EdcFont::decodeGlyph(int idx, std::vector<uint8_t>& out,
                          int& w, int& h, int& xo, int& yo, int& adv) {
    if (!loaded_ || idx < 0 || idx >= (int)char_count_) return false;
    const Entry& e = entries_[idx];

    // 命中字形缓存：免 SD 读 + 免霍夫曼解码
    auto it = cache_.find(e.cp);
    if (it != cache_.end()) {
        const GCEnt& g = it->second;
        w = g.w; h = g.h; xo = g.xo; yo = g.yo; adv = g.adv;
        out.assign(g.pix, g.pix + (size_t)w * h);
        return true;
    }

    w = e.w; h = e.h; xo = e.xo; yo = e.yo; adv = e.adv;
    out.assign((size_t)w * h, 15);  // 默认白
    if (e.size == 0 || w == 0 || h == 0) return true;  // 空白字符

    uint8_t* bmp = (uint8_t*)allocPsram(e.size);
    if (!bmp) return false;
    bool okr = readAt(e.off, bmp, e.size);
    if (!okr) { heap_caps_free(bmp); return false; }

    // 霍夫曼位流解码（MSB first）：
    //   "0"       → 白(15)
    //   "10"      → 黑(0)
    //   "11"+4bit → 灰(1..14)
    int total = w * h;
    int px = 0;
    size_t bytepos = 0;
    int bitpos = 7;
    auto readBit = [&]() -> int {
        if (bytepos >= e.size) return -1;
        int b = (bmp[bytepos] >> bitpos) & 1;
        if (--bitpos < 0) { bitpos = 7; bytepos++; }
        return b;
    };
    while (px < total) {
        int b0 = readBit();
        if (b0 < 0) break;
        if (b0 == 0) { out[px++] = 15; continue; }
        int b1 = readBit();
        if (b1 < 0) break;
        if (b1 == 0) { out[px++] = 0; continue; }
        // "11" + 4bit 灰度
        int g = 0;
        for (int k = 0; k < 4; k++) {
            int bb = readBit();
            if (bb < 0) break;
            g = (g << 1) | bb;
        }
        out[px++] = (uint8_t)g;
    }
    heap_caps_free(bmp);

    // 写入缓存（预算超出整体清空——书是顺序读的，偶发清空无伤）
    size_t pixBytes = (size_t)w * h;
    if (cache_bytes_ + pixBytes > CACHE_CAP) {
        for (auto& kv : cache_) heap_caps_free(kv.second.pix);
        cache_.clear();
        cache_bytes_ = 0;
    }
    uint8_t* p = (uint8_t*)allocPsram(pixBytes);
    if (p) {
        memcpy(p, out.data(), pixBytes);
        cache_.emplace(e.cp, GCEnt{w, h, xo, yo, adv, p});
        cache_bytes_ += pixBytes;
    }
    return true;
}

int EdcFont::textWidth(const char* utf8) {
    if (!loaded_ || !utf8) return 0;
    const uint8_t* p = (const uint8_t*)utf8;
    int width = 0;
    while (*p) {
        uint32_t cp = nextCodepoint(p);
        if (cp == 0) break;
        int idx = findGlyph((uint16_t)cp);
        width += (idx >= 0) ? entries_[idx].adv : font_height_ / 2;
    }
    return width;
}

int EdcFont::drawOnce(PortraitCanvas& canvas, const char* utf8,
                      int x, int y, int scale, uint8_t gray) {
    if (!loaded_ || !utf8) return x;
    int cx = x;
    const uint8_t* p = (const uint8_t*)utf8;
    std::vector<uint8_t> pix;
    while (*p) {
        uint32_t cp = nextCodepoint(p);
        if (cp == 0) break;
        int idx = findGlyph((uint16_t)cp);
        if (idx < 0) { cx += (font_height_ / 2) * scale; continue; }
        int w, h, xo, yo, adv;
        if (!decodeGlyph(idx, pix, w, h, xo, yo, adv)) {
            cx += (adv > 0 ? adv : font_height_ / 2) * scale;
            continue;
        }
        if (gray == 0) {
            // 默认：按字形灰度绘制（g: 0=黑..15=白），白(15)=背景跳过
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    uint8_t g = pix[(size_t)row * w + col];
                    if (g >= 15) continue;
                    int px = cx + xo * scale + col * scale;
                    int py = y + yo * scale + row * scale;
                    if (scale == 1) {
                        canvas.drawPixelGray(px, py, g);
                    } else {
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                canvas.drawPixelGray(px + sx, py + sy, g);
                    }
                }
            }
        } else {
            // 统一灰度：所有墨色像素渲染为 gray（深灰辅助文字）
            for (int row = 0; row < h; row++) {
                for (int col = 0; col < w; col++) {
                    if (pix[(size_t)row * w + col] >= 15) continue;
                    int px = cx + xo * scale + col * scale;
                    int py = y + yo * scale + row * scale;
                    if (scale == 1) {
                        canvas.drawPixelGray(px, py, gray);
                    } else {
                        for (int sy = 0; sy < scale; sy++)
                            for (int sx = 0; sx < scale; sx++)
                                canvas.drawPixelGray(px + sx, py + sy, gray);
                    }
                }
            }
        }
        cx += (adv > 0 ? adv : w) * scale;
    }
    return cx;
}

int EdcFont::drawText(PortraitCanvas& canvas, const char* utf8,
                      int x, int y, int scale, bool bold) {
    if (scale < 1) scale = 1;
    int endX = drawOnce(canvas, utf8, x, y, scale, 0);
    // 仿粗体：位图字体没有真粗体，x+1 二次绘制
    if (bold) drawOnce(canvas, utf8, x + 1, y, scale, 0);
    return endX;
}

int EdcFont::drawTextGray(PortraitCanvas& canvas, const char* utf8,
                          int x, int y, int scale, uint8_t gray) {
    if (scale < 1) scale = 1;
    if (gray > 15) gray = 15;
    return drawOnce(canvas, utf8, x, y, scale, gray);
}

}  // namespace eego
