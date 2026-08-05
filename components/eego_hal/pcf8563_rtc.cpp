// EEGO A4 RTC 实现 — PCF8563 I2C 实时时钟
// 寄存器布局（PCF8563 数据手册）:
//   0x00: Control/Status 1
//   0x01: Control/Status 2
//   0x02: VL 秒 (bit7=VL 标志, bit6-0=秒 BCD)
//   0x03: 分钟 BCD
//   0x04: 小时 BCD
//   0x05: 日期 BCD
//   0x06: 星期 BCD
//   0x07: 月 BCD (bit7=世纪: 1=1900s, 0=2000s)
//   0x08: 年 BCD
//   0x0D: CLKOUT 控制
#include "pcf8563_rtc.h"

#include <esp_log.h>
#include <cstring>

static const char* RTC_TAG = "eego_rtc";

// PCF8563 寄存器地址
static constexpr uint8_t PCF8563_REG_CONTROL_STATUS1 = 0x00;
static constexpr uint8_t PCF8563_REG_CONTROL_STATUS2 = 0x01;
static constexpr uint8_t PCF8563_REG_VL_SECONDS      = 0x02;  // 时间起始寄存器
static constexpr uint8_t PCF8563_REG_CLKOUT          = 0x0D;
static constexpr uint8_t PCF8563_CLKOUT_DISABLED     = 0x00;
static constexpr uint8_t PCF8563_VL_FLAG             = 0x80;  // 秒寄存器 bit7

// 确保共享 I2C 总线已安装
static esp_err_t rtcEnsureI2cBus(i2c_port_t port) {
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = static_cast<gpio_num_t>(eego::I2C_SDA);
    conf.scl_io_num = static_cast<gpio_num_t>(eego::I2C_SCL);
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = eego::I2C_HZ;
    esp_err_t err = i2c_param_config(port, &conf);
    if (err != ESP_OK) return err;
    err = i2c_driver_install(port, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    return ESP_OK;
}

namespace eego {

Pcf8563Rtc::Pcf8563Rtc(i2c_port_t port) : port_(port) {}

uint8_t Pcf8563Rtc::bcdToBin(uint8_t bcd) {
    return static_cast<uint8_t>((bcd >> 4) * 10U + (bcd & 0x0FU));
}

uint8_t Pcf8563Rtc::binToBcd(uint8_t bin) {
    return static_cast<uint8_t>(((bin / 10U) << 4) | (bin % 10U));
}

esp_err_t Pcf8563Rtc::readReg(uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_write_read_device(
        port_, PCF8563_ADDRESS, &reg, 1, data, len, pdMS_TO_TICKS(200));
}

esp_err_t Pcf8563Rtc::writeReg(uint8_t reg, const uint8_t* data, size_t len) {
    // 写操作：设备地址 + 寄存器地址 + 数据
    uint8_t buf[16];
    if (len + 1 > sizeof(buf)) return ESP_ERR_INVALID_SIZE;
    buf[0] = reg;
    memcpy(&buf[1], data, len);
    return i2c_master_write_to_device(
        port_, PCF8563_ADDRESS, buf, len + 1, pdMS_TO_TICKS(200));
}

esp_err_t Pcf8563Rtc::begin() {
    esp_err_t err = rtcEnsureI2cBus(port_);
    if (err != ESP_OK) {
        ESP_LOGE(RTC_TAG, "I2C bus init failed: %s", esp_err_to_name(err));
        return err;
    }

    // 探测 PCF8563：读 Control/Status 1 寄存器
    uint8_t status1 = 0;
    err = readReg(PCF8563_REG_CONTROL_STATUS1, &status1, 1);
    if (err != ESP_OK) {
        ESP_LOGE(RTC_TAG, "PCF8563 not responding at 0x%02X: %s",
                 PCF8563_ADDRESS, esp_err_to_name(err));
        return err;
    }

    // 禁用 CLKOUT（不需要 32kHz 方波输出）
    uint8_t clkoutOff = PCF8563_CLKOUT_DISABLED;
    writeReg(PCF8563_REG_CLKOUT, &clkoutOff, 1);

    ESP_LOGI(RTC_TAG, "PCF8563 initialized at 0x%02X", PCF8563_ADDRESS);
    return ESP_OK;
}

RtcTime Pcf8563Rtc::readTime() {
    RtcTime t;
    memset(&t, 0, sizeof(t));
    t.valid = false;

    // 从 0x02 开始连续读 7 字节：秒/分/时/日/周/月/年
    uint8_t raw[7];
    memset(raw, 0, sizeof(raw));
    esp_err_t err = readReg(PCF8563_REG_VL_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGW(RTC_TAG, "RTC read failed: %s", esp_err_to_name(err));
        return t;
    }

    // VL 标志：秒寄存器 bit7。VL=1 表示时钟停振/电池耗尽，时间不可靠
    bool vl = (raw[0] & PCF8563_VL_FLAG) != 0;
    t.valid = !vl;

    // BCD → 二进制
    t.second  = bcdToBin(raw[0] & 0x7FU);
    t.minute  = bcdToBin(raw[1] & 0x7FU);
    t.hour    = bcdToBin(raw[2] & 0x3FU);
    t.day     = bcdToBin(raw[3] & 0x3FU);
    t.weekday = bcdToBin(raw[4] & 0x07U);
    t.month   = bcdToBin(raw[5] & 0x1FU);

    // 世纪位 bit7：1=1900s, 0=2000s
    uint8_t yy = bcdToBin(raw[6]);
    t.year = (raw[5] & 0x80U) ? static_cast<uint16_t>(1900 + yy)
                              : static_cast<uint16_t>(2000 + yy);

    // VL 置位时清除：写回秒值（bit7=0），恢复振荡器可靠性标志
    if (vl) {
        uint8_t secClear = binToBcd(t.second) & 0x7FU;  // 清除 VL 位
        writeReg(PCF8563_REG_VL_SECONDS, &secClear, 1);
        ESP_LOGW(RTC_TAG, "RTC VL flag was set (oscillator stopped), cleared");
    }

    return t;
}

esp_err_t Pcf8563Rtc::writeTime(const RtcTime& t) {
    uint8_t raw[7];
    // 写秒值时 bit7=0 自动清除 VL 标志
    raw[0] = binToBcd(t.second) & 0x7FU;                  // 秒 (VL=0)
    raw[1] = binToBcd(t.minute) & 0x7FU;                   // 分
    raw[2] = binToBcd(t.hour) & 0x3FU;                     // 时 (24h)
    raw[3] = binToBcd(t.day) & 0x3FU;                      // 日
    raw[4] = binToBcd(t.weekday) & 0x07U;                  // 星期
    // 月：bit7=世纪 (year<2000 → 1=1900s, year>=2000 → 0=2000s)
    uint8_t centuryBit = (t.year < 2000) ? 0x80U : 0x00U;
    raw[5] = static_cast<uint8_t>(binToBcd(t.month) | centuryBit);
    raw[6] = binToBcd(static_cast<uint8_t>(t.year % 100));  // 年（2位）

    esp_err_t err = writeReg(PCF8563_REG_VL_SECONDS, raw, sizeof(raw));
    if (err != ESP_OK) {
        ESP_LOGE(RTC_TAG, "RTC write failed: %s", esp_err_to_name(err));
    }
    return err;
}

time_t Pcf8563Rtc::toEpoch(const RtcTime& t) {
    struct tm tm;
    memset(&tm, 0, sizeof(tm));
    tm.tm_year = t.year - 1900;
    tm.tm_mon = t.month - 1;
    tm.tm_mday = t.day;
    tm.tm_hour = t.hour;
    tm.tm_min = t.minute;
    tm.tm_sec = t.second;
    tm.tm_wday = t.weekday;
    // mktime 使用本地时区；调用方可通过 setenv("TZ","UTC") 设置 UTC
    return mktime(&tm);
}

}  // namespace eego
