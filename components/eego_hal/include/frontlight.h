// EEGO A4 前光驱动 — LM3630A (I2C 0x36)，GPIO12 使能
// 纯 ESP-IDF，使用 legacy driver/i2c.h API
// 寄存器序列参考 TI SNVS974B
#pragma once
#include <stdint.h>
#include "eego_pins.h"
#include <esp_err.h>
#include <driver/gpio.h>
#include <driver/i2c.h>

namespace eego {

class FrontlightManager {
public:
    static FrontlightManager& instance();

    bool present();                       // 探测前光是否存在
    void setBrightness(uint8_t percent);  // 0-100，0=关闭前光
    void setWarm(uint8_t warmPercent);    // 0=全冷，100=全暖
    void off();                           // 关闭（等效 setBrightness(0)）
    void on();                            // 恢复上一次亮度

private:
    FrontlightManager() = default;

    uint8_t _brightness = 0;
    uint8_t _warmPercent = 0;
    bool _i2cConfigured = false;
    bool _presentCached_ = false;
    bool _present_ = false;

    esp_err_t lm3630aWrite(uint8_t reg, uint8_t value);
    esp_err_t lm3630aRead(uint8_t reg, uint8_t* value);
    esp_err_t lm3630aUpdate(uint8_t reg, uint8_t mask, uint8_t value);
    bool configureLm3630a();
    void applyLm3630a();
};

}  // namespace eego