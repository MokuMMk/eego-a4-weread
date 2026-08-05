// GSLX680 电容触控驱动（EEGO A4）— 纯 ESP-IDF
//
// 轮询式触控驱动：调用者每 8ms 调用一次 poll() 读取 24 字节触控帧。
// 坐标经 eego_pins.h 的 rawTouchToLogicalX/Y 映射为 portrait 逻辑坐标。
// 屏下电容键（raw_x≈0x03a0, raw_y≈0x1020 sentinel）按短按/长按区分。
#pragma once
#include "eego_pins.h"
#include <stdint.h>
#include <esp_err.h>
#include "driver/i2c.h"

namespace eego {

enum class TouchEventType { None, Touch, FrontKeyShort, FrontKeyLong };

struct TouchEvent {
    TouchEventType type = TouchEventType::None;
    uint16_t x = 0;       // portrait 逻辑坐标
    uint16_t y = 0;
    bool pressed = false;  // true=按下, false=抬起
};

class Gslx680Touch {
public:
    Gslx680Touch();
    ~Gslx680Touch();

    esp_err_t begin(i2c_port_t port = I2C_NUM_0);  // 初始化 I²C + 上传固件
    void end();

    TouchEvent poll();  // 8ms 轮询，返回事件

    bool isPressed() const { return pressed_; }

private:
    esp_err_t writeReg(uint8_t reg, const uint8_t* data, size_t len);
    esp_err_t writeReg32(uint8_t reg, uint32_t value);
    esp_err_t readReg(uint8_t reg, uint8_t* data, size_t len);
    esp_err_t uploadFirmware();
    void clearRegisters();
    void resetChip();
    void startChip();
    bool checkChip();

    i2c_port_t i2cPort_ = I2C_NUM_0;
    bool initialized_ = false;  // I²C 总线已安装
    bool enabled_ = false;      // 固件上传并校验通过
    bool pressed_ = false;
    uint16_t lastRawX_ = 0;
    uint16_t lastRawY_ = 0;
    int64_t frontKeyDownTime_ = 0;  // esp_timer_get_time() 微秒
    bool frontKeyDown_ = false;
    bool frontKeyLongDelivered_ = false;
};

}  // namespace eego
