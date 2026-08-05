// GSLX680 电容触控驱动实现（EEGO A4）— 纯 ESP-IDF
//
// 协议说明：EEGO A4 的 GSLX680 固件表为 {页内 offset, 32 位 value} 结构，
// 上传时通过 0xf0 寄存器选页，再将页内连续寄存器（offset 递增 4）批量写入。
// 这与通用 GSL1680 的 0xbc/0xbd/0xbe 单字节流式上传不同，是本设备的实际协议，
// 移植自 eego-a4-template-ref/lib/InputManager 的 gslx680* 实现。
#include "gslx680_touch.h"
#include "gslx680_firmware.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>

namespace eego {

namespace {
// 最少 1 个 tick 的毫秒延时（兼容 100Hz tick 配置下 pdMS_TO_TICKS 小于 1 的情况）
void delayMs(uint32_t ms) {
    TickType_t ticks = pdMS_TO_TICKS(ms);
    if (ticks == 0) ticks = 1;
    vTaskDelay(ticks);
}
}  // namespace

Gslx680Touch::Gslx680Touch() {}

Gslx680Touch::~Gslx680Touch() { end(); }

// --- I²C 原语 ---

esp_err_t Gslx680Touch::writeReg(uint8_t reg, const uint8_t* data, size_t len) {
    if (len == 0 || data == nullptr) {
        return i2c_master_write_to_device(i2cPort_, GSLX680_ADDRESS, &reg, 1,
                                          pdMS_TO_TICKS(100));
    }
    // 固件批量写入最大 64 字节 + 1 寄存器字节
    uint8_t buf[65];
    buf[0] = reg;
    memcpy(buf + 1, data, len);
    return i2c_master_write_to_device(i2cPort_, GSLX680_ADDRESS, buf, len + 1,
                                      pdMS_TO_TICKS(100));
}

esp_err_t Gslx680Touch::writeReg32(uint8_t reg, uint32_t value) {
    uint8_t data[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    return writeReg(reg, data, 4);
}

esp_err_t Gslx680Touch::readReg(uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_write_read_device(i2cPort_, GSLX680_ADDRESS, &reg, 1, data,
                                        len, pdMS_TO_TICKS(100));
}

// --- 芯片控制（移植自参考代码 gslx680ClearRegisters/ResetChip/StartChip/Check）---

void Gslx680Touch::clearRegisters() {
    writeReg32(0xe0, 0x00000088);
    delayMs(20);
    writeReg32(0x80, 0x00000003);
    delayMs(5);
    writeReg32(0xe4, 0x00000004);
    delayMs(5);
    writeReg32(0xe0, 0x00000000);
    delayMs(20);
}

void Gslx680Touch::resetChip() {
    writeReg32(0xe0, 0x00000088);
    delayMs(20);
    writeReg32(0xe4, 0x00000004);
    delayMs(10);
    writeReg32(0xbc, 0x00000000);
    delayMs(10);
}

void Gslx680Touch::startChip() {
    writeReg32(0xe0, 0x00000000);
    delayMs(10);
}

bool Gslx680Touch::checkChip() {
    uint8_t status[4] = {};
    if (readReg(0xb0, status, sizeof(status)) != ESP_OK) return false;
    return status[0] == 0x5a && status[1] == 0x5a && status[2] == 0x5a &&
           status[3] == 0x5a;
}

// --- 固件上传 ---
// offset==0xf0：页选择，直接写 32 位值到 0xf0。
// 其余：将最多 16 个连续 32 位寄存器（offset 递增 4）合并为一次 I²C 写入，
//      起始寄存器为首个 offset。参考代码的 OEM 批量优化。

esp_err_t Gslx680Touch::uploadFirmware() {
    size_t i = 0;
    while (i < GSLX680_FIRMWARE_COUNT) {
        const Gslx680FirmwareEntry& entry = GSLX680_FIRMWARE[i];
        if (entry.offset == 0xf0) {
            if (writeReg32(entry.offset, entry.value) != ESP_OK) return ESP_FAIL;
            ++i;
            continue;
        }

        uint8_t payload[64];
        uint8_t words = 0;
        const uint8_t firstReg = entry.offset;
        while (i < GSLX680_FIRMWARE_COUNT && words < 16) {
            const Gslx680FirmwareEntry& e = GSLX680_FIRMWARE[i];
            const uint8_t expected = static_cast<uint8_t>(firstReg + words * 4);
            if (e.offset == 0xf0 || e.offset != expected) break;
            payload[words * 4] = static_cast<uint8_t>(e.value);
            payload[words * 4 + 1] = static_cast<uint8_t>(e.value >> 8);
            payload[words * 4 + 2] = static_cast<uint8_t>(e.value >> 16);
            payload[words * 4 + 3] = static_cast<uint8_t>(e.value >> 24);
            ++words;
            ++i;
        }
        if (words == 0) {
            ++i;  // 安全跳过非预期条目
            continue;
        }
        if (writeReg(firstReg, payload, static_cast<size_t>(words) * 4) != ESP_OK)
            return ESP_FAIL;
    }
    return ESP_OK;
}

// --- 生命周期 ---

esp_err_t Gslx680Touch::begin(i2c_port_t port) {
    i2cPort_ = port;

    // 配置共享 I²C 总线（SDA=2, SCL=1, 400kHz）
    i2c_config_t conf;
    memset(&conf, 0, sizeof(conf));
    conf.mode = I2C_MODE_MASTER;
    conf.sda_io_num = static_cast<gpio_num_t>(I2C_SDA);
    conf.scl_io_num = static_cast<gpio_num_t>(I2C_SCL);
    conf.sda_pullup_en = GPIO_PULLUP_ENABLE;
    conf.scl_pullup_en = GPIO_PULLUP_ENABLE;
    conf.master.clk_speed = I2C_HZ;
    esp_err_t err = i2c_param_config(i2cPort_, &conf);
    if (err != ESP_OK) return err;

    // 安装驱动；若已安装（与 PCF8563 共享 I2C_NUM_0）则视为成功
    err = i2c_driver_install(i2cPort_, I2C_MODE_MASTER, 0, 0, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) return err;
    initialized_ = true;

    // 探测并演练页选择寄存器（与 CrossLink 一致）
    uint8_t probe = 0;
    if (readReg(0xf0, &probe, 1) != ESP_OK ||
        writeReg32(0xf0, 0x00000012) != ESP_OK ||
        readReg(0xf0, &probe, 1) != ESP_OK) {
        enabled_ = false;
        return ESP_FAIL;
    }

    // 复位/清寄存器序列
    clearRegisters();
    resetChip();
    resetChip();
    clearRegisters();
    resetChip();

    // 上传固件并启动
    if (uploadFirmware() != ESP_OK) {
        enabled_ = false;
        return ESP_FAIL;
    }
    startChip();
    resetChip();
    startChip();

    // 校验：0xb0 应返回 0x5a5a5a5a
    if (!checkChip()) {
        resetChip();
        startChip();
    }
    enabled_ = checkChip();
    return enabled_ ? ESP_OK : ESP_FAIL;
}

void Gslx680Touch::end() {
    enabled_ = false;
    pressed_ = false;
    frontKeyDown_ = false;
    frontKeyLongDelivered_ = false;
    // 不卸载共享 I²C 总线（与 PCF8563 共用 I2C_NUM_0）
}

// --- 轮询 ---
// 每 8ms 调用一次。读 0x80 的 24 字节帧：
//   frame[0] = 触点数；point0 的 raw_y 在 bytes[4-5]、raw_x 在 bytes[6-7]（小端）。
// 屏下电容键 sentinel：raw_x==0x03a0 且 raw_y==0x1020。

TouchEvent Gslx680Touch::poll() {
    TouchEvent ev;
    if (!enabled_) return ev;

    uint8_t frame[24] = {};
    if (readReg(0x80, frame, sizeof(frame)) != ESP_OK) return ev;

    const int64_t now = esp_timer_get_time();
    const uint8_t count = frame[0] > 5 ? 5 : frame[0];
    const uint16_t rawYWord =
        (static_cast<uint16_t>(frame[5]) << 8) | frame[4];
    const uint16_t rawXWord =
        (static_cast<uint16_t>(frame[7]) << 8) | frame[6];
    const bool homeKey = (count == 1 && rawXWord == FRONT_KEY_RAW_X &&
                          rawYWord == FRONT_KEY_RAW_Y);

    if (count == 0) {
        // 全部释放：结算屏下键短按，或触控抬起
        if (frontKeyDown_) {
            if (!frontKeyLongDelivered_) ev.type = TouchEventType::FrontKeyShort;
            frontKeyDown_ = false;
            frontKeyLongDelivered_ = false;
        } else if (pressed_) {
            ev.type = TouchEventType::Touch;
            ev.pressed = false;
            ev.x = rawTouchToLogicalX(lastRawY_);
            ev.y = rawTouchToLogicalY(lastRawX_);
            pressed_ = false;
        }
        return ev;
    }

    if (homeKey) {
        if (!frontKeyDown_) {
            frontKeyDown_ = true;
            frontKeyLongDelivered_ = false;
            frontKeyDownTime_ = now;
        } else if (!frontKeyLongDelivered_ &&
                   (now - frontKeyDownTime_) >=
                       static_cast<int64_t>(FRONT_KEY_LONG_MS) * 1000) {
            frontKeyLongDelivered_ = true;
            ev.type = TouchEventType::FrontKeyLong;
        }
        // 屏下键期间取消任何触控
        pressed_ = false;
        return ev;
    }

    // 普通触控：若之前屏下键按下，先结算短按
    if (frontKeyDown_) {
        const bool wasLong = frontKeyLongDelivered_;
        frontKeyDown_ = false;
        frontKeyLongDelivered_ = false;
        pressed_ = false;
        if (!wasLong) {
            ev.type = TouchEventType::FrontKeyShort;
            return ev;  // 本周期返回短按，触控下一周期处理
        }
        // 长按已交付，仅清理后继续处理当前触控
    }

    // 按住期间：仅交付一次按下事件（边沿触发）。
    // 若已处于按下状态（pressed_==true）则本次轮询为按住重复，返回空事件，
    // 避免一次按压被上层重复处理（如主页彩蛋计数、阅读页连翻多页）。
    if (pressed_) {
        return ev;  // ev 为空事件（type=None），上层忽略
    }

    const uint16_t rawY = rawYWord & 0x0fff;
    const uint16_t rawX = rawXWord & 0x0fff;
    lastRawX_ = rawX;
    lastRawY_ = rawY;
    ev.type = TouchEventType::Touch;
    ev.pressed = true;
    ev.x = rawTouchToLogicalX(rawY);
    ev.y = rawTouchToLogicalY(rawX);
    pressed_ = true;
    return ev;
}

}  // namespace eego
