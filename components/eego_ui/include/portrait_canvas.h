// PortraitCanvas — 逻辑 portrait 坐标 (552×768) 高级绘图 API
// 封装 Uc8279cDisplay 的 framebuffer 操作，提供安全区裁剪与原生四级灰阶。
// 纯 ESP-IDF，无 Arduino 依赖。Task 6。
#pragma once

#include "uc8279c_display.h"
#include "eego_pins.h"

#include <stdint.h>

namespace eego {

class PortraitCanvas {
public:
    explicit PortraitCanvas(Uc8279cDisplay& display);

    // 清屏（白底，覆盖整个物理面板，不受裁剪限制）
    void clear();

    // 基本绘图（color: 0=黑, 1=白；灰度 0-15 通过 drawPixelGray 写入原生灰阶平面）
    void drawPixel(int16_t x, int16_t y, uint8_t color);
    void drawRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color);
    void drawLine(int16_t x0, int16_t y0, int16_t x1, int16_t y1, uint8_t color);
    void drawCircle(int16_t cx, int16_t cy, int16_t r, uint8_t color);
    void fillCircle(int16_t cx, int16_t cy, int16_t r, uint8_t color);

    // 圆角矩形（r 为圆角半径，自动钳制到短边一半）
    void drawRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
    void fillRoundRect(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t color);
    // 圆角灰底填充（gray 0-15，用原生四级灰阶实现灰度圆角区域）
    void fillRoundRectGray(int16_t x, int16_t y, int16_t w, int16_t h, int16_t r, uint8_t gray);

    // 1-bpp 位图绘制（MSB-first，bit=1 画 color，bit=0 透明跳过）
    // icon 指向 width/8 字节/行 的紧凑位图，w/h 为图标尺寸。
    void drawIcon(int16_t x, int16_t y, const uint8_t* icon, int16_t w, int16_t h, uint8_t color);

    // 文字对齐绘制（通过 FontManager 测量 UI 字体宽度后偏移；y 为文本顶部）
    // drawTextCentered: 在 [x, x+w) 区间水平居中
    void drawTextCentered(int16_t x, int16_t y, int16_t w, const char* text, int16_t scale, bool bold);
    // drawTextRight: 在 [x, x+w) 区间右对齐
    void drawTextRight(int16_t x, int16_t y, int16_t w, const char* text, int16_t scale, bool bold);

    // 灰度值(0-15, 0=黑/15=白)写入驱动 MSB/LSB 灰阶平面（原生四级灰阶）
    // 0-3=黑, 4-7=深灰, 8-11=浅灰, 12-15=白；含灰度内容的帧刷新时走 displayGray
    void drawPixelGray(int16_t x, int16_t y, uint8_t gray);

    // 安全区裁剪
    void setClip(int16_t x, int16_t y, int16_t w, int16_t h);
    void resetClip();           // 恢复到全屏 (0,0,552,768)
    void useUiContentRect();    // 使用 UI 矩形区 (28,28,496×712)

    // 刷新
    void display(RefreshMode mode = RefreshMode::Fast);
    void displayAsync(RefreshMode mode = RefreshMode::Fast);
    void waitRefreshComplete();

    // 几何常量
    static constexpr uint16_t WIDTH = 552;
    static constexpr uint16_t HEIGHT = 768;

private:
    Uc8279cDisplay& display_;
    int16_t clipX_, clipY_, clipW_, clipH_;
    bool clipEnabled_;

    bool inClip(int16_t x, int16_t y) const;

    // 圆角矩形单角四分之一圆弧（描边用）
    // quadrant: 0=左上, 1=右上, 2=左下, 3=右下
    void drawCornerArc(int16_t cx, int16_t cy, int16_t r, int quadrant, uint8_t color);
};

}  // namespace eego
