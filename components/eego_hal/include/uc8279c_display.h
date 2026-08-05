#pragma once

// UC8279C 墨水屏显示驱动 — 纯 ESP-IDF 实现
// 面板控制器 768×600，固件 framebuffer 768×552 + 48 行白填充
// 逻辑 portrait 坐标 (x∈[0,551], y∈[0,767]) 映射到 native (nativeX=y, nativeY=551-x)
// BUSY active-low: 忙时 LOW，空闲 HIGH
// 0=黑, 1=白

#include "eego_pins.h"

#include <driver/spi_master.h>
#include <esp_err.h>
#include <stdint.h>

namespace eego {

enum class RefreshMode { Fast, Half, Full };

class Uc8279cDisplay {
public:
    Uc8279cDisplay();
    ~Uc8279cDisplay();

    esp_err_t begin();        // 初始化 SPI + GPIO + 控制器
    void end();               // 释放资源

    // framebuffer 操作（逻辑 portrait 坐标）
    void clear();             // 全白
    void fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color); // color: 0=黑 1=白
    void drawPixel(int16_t x, int16_t y, uint8_t color);  // color: 0=黑 1=白
    uint8_t getPixel(int16_t x, int16_t y) const;

    // 四级灰阶 (gray 0-15: 0-3=黑, 4-7=深灰, 8-11=浅灰, 12-15=白)
    // 首次调用延迟分配 MSB/LSB 灰阶缓冲并从 fb_ 同步；hasGrayContent() 为真时
    // drawPixel/fillRect 也会同步更新灰阶缓冲，保持 BW 与灰阶平面一致。
    void drawPixelGray(int16_t x, int16_t y, uint8_t gray);
    void clearGray();  // 重置灰阶状态（下次 drawPixelGray 重新同步 fb_）
    bool hasGrayContent() const { return hasGray_; }

    // 刷新
    void display(RefreshMode mode = RefreshMode::Fast, bool turnOff = false);
    void displayAsync(RefreshMode mode = RefreshMode::Fast);  // 非阻塞推送
    void waitRefreshComplete();
    // 四级灰阶刷新：用内部 MSB/LSB 缓冲，MSB→DTM1, LSB→DTM2，加载灰阶 LUT。
    // async=true 时仅发起刷新不等待 BUSY（turnOff 被忽略）。
    void displayGray(bool turnOff = false, bool async = false);

    // 电源
    void deepSleep();

    // 几何
    static constexpr uint16_t WIDTH = LOGICAL_WIDTH;   // 552
    static constexpr uint16_t HEIGHT = LOGICAL_HEIGHT; // 768

    uint8_t* framebuffer() { return fb_; }

private:
    esp_err_t spiInit();
    void gpioInit();
    void hardwareReset();
    void initController(bool grayMode = false);
    void ensurePowerOn();
    void fillControllerRam(uint8_t cmd, uint8_t fill);
    void loadFullLut();
    void loadFastLut();
    void loadGrayLut();
    void writeFrame(uint8_t cmd, const uint8_t* fb);
    // 发起刷新并等待完成；返回 false 表示刷新命令未被控制器接受（BUSY 未拉低）
    bool refresh(bool turnOff);
    void waitBusy(const char* tag);
    void sendCmd(uint8_t cmd);
    void sendData(uint8_t data);
    void sendData(const uint8_t* data, size_t len);
    void cmdData(uint8_t cmd, const uint8_t* data, size_t len);
    void spiTransmit(const uint8_t* data, size_t len);
    void ensureGrayBuffers();  // 延迟分配 + 首次从 fb_ 同步灰阶平面

    spi_device_handle_t spi_ = nullptr;
    uint8_t* fb_ = nullptr;           // PSRAM framebuffer, native 方向 (768×552/8)
    uint8_t* fbGrayMsb_ = nullptr;    // PSRAM 灰阶 MSB 平面 (与 fb_ 同尺寸)
    uint8_t* fbGrayLsb_ = nullptr;    // PSRAM 灰阶 LSB 平面 (与 fb_ 同尺寸)
    bool hasGray_ = false;            // 当前帧是否含灰度内容（灰阶平面有效）
    bool screenOn_ = false;
    bool grayControllerMode_ = false; // 控制器当前是否处于灰阶模式
    uint8_t fastRefreshesSinceFull_ = 0;
    static constexpr uint8_t MAX_CONSECUTIVE_FAST = 4;
};

}  // namespace eego
