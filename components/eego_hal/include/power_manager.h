// EEGO A4 电源管理 — GPIO4 锁存 + 电池 ADC + 充电状态
// 纯 ESP-IDF，无 Arduino 依赖
#pragma once
#include "eego_pins.h"
#include <esp_err.h>
#include <esp_adc/adc_oneshot.h>
#include <esp_adc/adc_cali.h>
#include <driver/gpio.h>

namespace eego {

struct BatteryInfo {
    uint16_t millivolts;     // 实际电池电压
    uint8_t percent;         // 0-100
    bool charging;           // 是否充电中
};

class PowerManager {
public:
    static PowerManager& instance();

    void holdPower();        // GPIO4 锁存（系统启动后首项操作！）
    BatteryInfo readBattery();
    bool isCharging();

private:
    PowerManager() = default;
    bool initialized_ = false;
    adc_oneshot_unit_handle_t adcUnit_ = nullptr;
    adc_cali_handle_t adcCali_ = nullptr;
    bool lm3630aPresent_ = false;
    bool chargeActiveHigh_ = false;

    void initAdc();
    void detectLm3630a();
    void ensureInitialized();
    static uint8_t percentFromMillivolts(uint16_t mv);
};

}  // namespace eego
