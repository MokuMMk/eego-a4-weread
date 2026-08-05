// font_manager.cpp — 字体管理器实现（Task 9）
#include "font_manager.h"

#include <cstring>
#include <new>

#include "esp_log.h"

#include "edcbook_font.h"
#include "cpfont_renderer.h"
#include "sd_card.h"

namespace eego {

static const char* TAG = "eego.fontmgr";

FontManager& FontManager::instance() {
    static FontManager inst;
    return inst;
}

FontManager::~FontManager() {
    unloadUi();
    unloadBody();
}

void FontManager::unloadUi() {
    if (uiEdc_) { delete uiEdc_; uiEdc_ = nullptr; }
    if (uiCp_)  { delete uiCp_;  uiCp_  = nullptr; }
    uiFmt_ = FontFormat::Unknown;
}

void FontManager::unloadBody() {
    if (bodyEdc_) { delete bodyEdc_; bodyEdc_ = nullptr; }
    if (bodyCp_)  { delete bodyCp_;  bodyCp_  = nullptr; }
    bodyFmt_ = FontFormat::Unknown;
}

FontFormat FontManager::detectFormat(const char* path) {
    if (!path) return FontFormat::Unknown;
    if (!SdCard::instance().ready()) return FontFormat::Unknown;
    FILE* f = SdCard::instance().open(path, "rb");
    if (!f) return FontFormat::Unknown;
    uint8_t magic[8] = {};
    size_t n = fread(magic, 1, sizeof(magic), f);
    fclose(f);
    if (n < 6) return FontFormat::EdcbookBin;  // 太短，按 .bin 处理（会加载失败）
    // EEFONT / CPFONT → CPFONT v4；其他 → EDCBook .bin
    if (memcmp(magic, "EEFONT", 6) == 0 || memcmp(magic, "CPFONT", 6) == 0) {
        return FontFormat::CpFontV4;
    }
    return FontFormat::EdcbookBin;
}

bool FontManager::loadInto(const char* path, FontFormat& fmtOut,
                           EdcFont*& edcOut, CpFontRenderer*& cpOut) {
    // 释放旧实例
    if (edcOut) { delete edcOut; edcOut = nullptr; }
    if (cpOut)  { delete cpOut;  cpOut = nullptr; }
    fmtOut = FontFormat::Unknown;

    FontFormat fmt = detectFormat(path);
    if (fmt == FontFormat::CpFontV4) {
        cpOut = new (std::nothrow) CpFontRenderer();
        if (!cpOut) return false;
        if (!cpOut->load(path)) { delete cpOut; cpOut = nullptr; return false; }
        fmtOut = FontFormat::CpFontV4;
        ESP_LOGI(TAG, "UI/Body 字体加载为 CPFONT v4: %s", path);
        return true;
    }
    if (fmt == FontFormat::EdcbookBin) {
        edcOut = new (std::nothrow) EdcFont();
        if (!edcOut) return false;
        if (!edcOut->load(path)) { delete edcOut; edcOut = nullptr; return false; }
        fmtOut = FontFormat::EdcbookBin;
        ESP_LOGI(TAG, "UI/Body 字体加载为 EDCBook .bin: %s", path);
        return true;
    }
    return false;
}

bool FontManager::loadUiFont(const char* path) {
    if (!path) return false;
    return loadInto(path, uiFmt_, uiEdc_, uiCp_);
}

bool FontManager::loadBodyFont(const char* path) {
    if (!path) return false;
    return loadInto(path, bodyFmt_, bodyEdc_, bodyCp_);
}

void FontManager::drawText(PortraitCanvas& canvas, int16_t x, int16_t y,
                           const char* utf8, int16_t scale, bool bold) {
    if (!utf8) return;
    if (uiFmt_ == FontFormat::CpFontV4 && uiCp_) {
        uiCp_->drawText(canvas, x, y, utf8, scale, bold);
    } else if (uiFmt_ == FontFormat::EdcbookBin && uiEdc_) {
        uiEdc_->drawText(canvas, utf8, x, y, scale, bold);
    }
}

void FontManager::drawTextBody(PortraitCanvas& canvas, int16_t x, int16_t y,
                               const char* utf8, int16_t scale, bool bold) {
    if (!utf8) return;
    if (bodyFmt_ == FontFormat::CpFontV4 && bodyCp_) {
        bodyCp_->drawText(canvas, x, y, utf8, scale, bold);
    } else if (bodyFmt_ == FontFormat::EdcbookBin && bodyEdc_) {
        bodyEdc_->drawText(canvas, utf8, x, y, scale, bold);
    }
}

void FontManager::drawTextGray(PortraitCanvas& canvas, int16_t x, int16_t y,
                               const char* utf8, int16_t scale, uint8_t gray) {
    if (!utf8) return;
    if (uiFmt_ == FontFormat::CpFontV4 && uiCp_) {
        uiCp_->drawTextGray(canvas, x, y, utf8, scale, gray);
    } else if (uiFmt_ == FontFormat::EdcbookBin && uiEdc_) {
        uiEdc_->drawTextGray(canvas, utf8, x, y, scale, gray);
    }
}

int16_t FontManager::uiFontHeight() const {
    if (uiFmt_ == FontFormat::CpFontV4 && uiCp_) return uiCp_->fontHeight();
    if (uiFmt_ == FontFormat::EdcbookBin && uiEdc_) return (int16_t)uiEdc_->fontHeight();
    return 0;
}

int16_t FontManager::bodyFontHeight() const {
    if (bodyFmt_ == FontFormat::CpFontV4 && bodyCp_) return bodyCp_->fontHeight();
    if (bodyFmt_ == FontFormat::EdcbookBin && bodyEdc_) return (int16_t)bodyEdc_->fontHeight();
    return 0;
}

int16_t FontManager::uiTextWidth(const char* utf8) const {
    if (!utf8) return 0;
    if (uiFmt_ == FontFormat::CpFontV4 && uiCp_) return uiCp_->textWidth(utf8);
    if (uiFmt_ == FontFormat::EdcbookBin && uiEdc_) return (int16_t)uiEdc_->textWidth(utf8);
    return 0;
}

int16_t FontManager::bodyTextWidth(const char* utf8) const {
    if (!utf8) return 0;
    if (bodyFmt_ == FontFormat::CpFontV4 && bodyCp_) return bodyCp_->textWidth(utf8);
    if (bodyFmt_ == FontFormat::EdcbookBin && bodyEdc_) return (int16_t)bodyEdc_->textWidth(utf8);
    return 0;
}

bool FontManager::uiLoaded() const {
    return (uiEdc_ && uiEdc_->loaded()) || (uiCp_ && uiCp_->loaded());
}

bool FontManager::bodyLoaded() const {
    return (bodyEdc_ && bodyEdc_->loaded()) || (bodyCp_ && bodyCp_->loaded());
}

}  // namespace eego
