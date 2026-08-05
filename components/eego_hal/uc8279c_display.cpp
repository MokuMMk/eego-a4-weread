// UC8279C 墨水屏显示驱动 — 纯 ESP-IDF 实现
// 移植自 eego-a4-template-ref/lib/FreeInkDisplay 的 Arduino 驱动
// 使用 driver/spi_master + driver/gpio，无 Arduino 依赖

#include "uc8279c_display.h"
#include "uc8279c_luts.h"

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_psram.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

static const char* TAG_UC = "eego.display";

namespace eego {
namespace {

// UC8279C 命令寄存器
constexpr uint8_t CMD_PANEL_SETTING = 0x00;
constexpr uint8_t CMD_POWER_SETTING = 0x01;
constexpr uint8_t CMD_POWER_OFF = 0x02;
constexpr uint8_t CMD_POWER_OFF_SEQUENCE = 0x03;
constexpr uint8_t CMD_POWER_ON = 0x04;
constexpr uint8_t CMD_BOOSTER_SOFT_START = 0x06;
constexpr uint8_t CMD_DEEP_SLEEP = 0x07;
constexpr uint8_t CMD_DTM1 = 0x10;
constexpr uint8_t CMD_DISPLAY_REFRESH = 0x12;
constexpr uint8_t CMD_DTM2 = 0x13;
constexpr uint8_t CMD_LUT_VCOM = 0x20;
constexpr uint8_t CMD_PLL_CONTROL = 0x30;
constexpr uint8_t CMD_VCOM_DATA_INTERVAL = 0x50;
constexpr uint8_t CMD_RESOLUTION = 0x61;
constexpr uint8_t CMD_GATE_SOURCE_SETTING = 0x65;
constexpr uint8_t CMD_VCOM_DC = 0x82;
constexpr uint8_t CMD_TCON = 0xe1;

// BUSY 等待超时: 30 秒
constexpr int64_t BUSY_TIMEOUT_US = 30 * 1000 * 1000LL;
// 等待 BUSY 进入忙态窗口: 100ms (控制器可能需要短暂时间才拉低 BUSY)
constexpr int64_t BUSY_SETTLE_US = 100 * 1000LL;
// 刷新启动确认窗口: 200ms (发 REFRESH 后 BUSY 应在此窗口内拉低)
constexpr int64_t BUSY_START_CONFIRM_US = 200 * 1000LL;
// 空闲确认时长: 30ms (BUSY 回 HIGH 后需保持空闲该时长才算真正刷新结束，
// 覆盖灰阶等多段刷新 BUSY 短暂释放又拉低的情况，防止提前返回导致上层打断刷新)
constexpr int64_t BUSY_IDLE_CONFIRM_MS = 30;

// gpio_num_t 辅助转换
inline gpio_num_t gn(int8_t pin) {
    return static_cast<gpio_num_t>(pin);
}

}  // namespace

// ── 构造 / 析构 ────────────────────────────────────────────────────────────

Uc8279cDisplay::Uc8279cDisplay() = default;

Uc8279cDisplay::~Uc8279cDisplay() {
    end();
}

// ── 生命周期 ────────────────────────────────────────────────────────────────

esp_err_t Uc8279cDisplay::begin() {
    // 诊断 PSRAM
    size_t psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t psram_total = esp_psram_get_size();
    ESP_LOGI(TAG_UC, "PSRAM total=%u free=%u, FB_SIZE=%u",
             (unsigned)psram_total, (unsigned)psram_free, (unsigned)FRAMEBUFFER_SIZE);

    // 分配 PSRAM framebuffer (native 方向: 768×552/8 = 52992 字节)
    // 幂等：唤醒时 fb_ 已存在，跳过重新分配以保留显示内容
    if (!fb_) {
        fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
        if (!fb_) {
            ESP_LOGE(TAG_UC, "PSRAM 分配失败! 尝试内部 RAM");
            fb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_INTERNAL));
            if (!fb_) {
                ESP_LOGE(TAG_UC, "内部 RAM 也分配失败!");
                return ESP_ERR_NO_MEM;
            }
        }
        ESP_LOGI(TAG_UC, "framebuffer @ %p (%u 字节)", fb_, (unsigned)FRAMEBUFFER_SIZE);
        clear();
    }

    gpioInit();
    esp_err_t ret = spiInit();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UC, "SPI 初始化失败: %s", esp_err_to_name(ret));
        return ret;
    }
    ESP_LOGI(TAG_UC, "spi_ handle = %p", spi_);

    // 电源时序: POWER_ENABLE 拉高 → 等 100ms → RST 时序
    gpio_set_level(gn(EPD_POWER_ENABLE), 1);
    vTaskDelay(pdMS_TO_TICKS(100));
    hardwareReset();

    initController();
    fastRefreshesSinceFull_ = 0;

    return ESP_OK;
}

void Uc8279cDisplay::end() {
    if (fb_) {
        free(fb_);
        fb_ = nullptr;
    }
    if (fbGrayMsb_) {
        free(fbGrayMsb_);
        fbGrayMsb_ = nullptr;
    }
    if (fbGrayLsb_) {
        free(fbGrayLsb_);
        fbGrayLsb_ = nullptr;
    }
    hasGray_ = false;
    if (spi_) {
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
    }
    spi_bus_free(SPI3_HOST);
    screenOn_ = false;
    grayControllerMode_ = false;
    fastRefreshesSinceFull_ = 0;
}

// ── GPIO 初始化 ─────────────────────────────────────────────────────────────

void Uc8279cDisplay::gpioInit() {
    // 释放 hold (deep sleep 时 power enable 可能被 hold 在 LOW)
    gpio_hold_dis(gn(EPD_POWER_ENABLE));

    // 输出引脚
    gpio_set_direction(gn(EPD_POWER_ENABLE), GPIO_MODE_OUTPUT);
    gpio_set_direction(gn(EPD_CS), GPIO_MODE_OUTPUT);
    gpio_set_direction(gn(EPD_DC), GPIO_MODE_OUTPUT);
    gpio_set_direction(gn(EPD_RESET), GPIO_MODE_OUTPUT);

    // BUSY 输入 + 上拉 (active-low)
    gpio_set_direction(gn(EPD_BUSY), GPIO_MODE_INPUT);
    gpio_pullup_en(gn(EPD_BUSY));

    // 初始电平
    gpio_set_level(gn(EPD_POWER_ENABLE), 0);  // 先关电
    gpio_set_level(gn(EPD_CS), 1);            // 片选无效
    gpio_set_level(gn(EPD_DC), 1);            // 数据模式
    gpio_set_level(gn(EPD_RESET), 1);         // 非复位
}

// ── SPI 初始化 ───────────────────────────────────────────────────────────────

esp_err_t Uc8279cDisplay::spiInit() {
    // 幂等：浅睡唤醒时 SPI bus 未释放，重复 spi_bus_initialize 会返回
    // ESP_ERR_INVALID_STATE，进而导致后续 waitBusy() 死锁。
    if (spi_) {
        ESP_LOGI(TAG_UC, "SPI 已初始化，跳过（handle=%p）", spi_);
        return ESP_OK;
    }
    spi_bus_config_t buscfg = {};
    buscfg.mosi_io_num = EPD_MOSI;
    buscfg.miso_io_num = -1;       // 无 MISO
    buscfg.sclk_io_num = EPD_SCLK;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 4096;

    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = static_cast<int>(EPD_SPI_HZ);  // 20MHz
    devcfg.mode = 0;                                         // SPI MODE 0
    devcfg.spics_io_num = -1;                                // 手动控制 CS
    devcfg.queue_size = 1;

    ESP_LOGI(TAG_UC, "SPI init: host=SPI3 SCLK=%d MOSI=%d CS=%d DC=%d RST=%d BUSY=%d",
             EPD_SCLK, EPD_MOSI, EPD_CS, EPD_DC, EPD_RESET, EPD_BUSY);

    esp_err_t ret = spi_bus_initialize(SPI3_HOST, &buscfg, SPI_DMA_CH_AUTO);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UC, "spi_bus_initialize failed: %s", esp_err_to_name(ret));
        return ret;
    }
    ret = spi_bus_add_device(SPI3_HOST, &devcfg, &spi_);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG_UC, "spi_bus_add_device failed: %s", esp_err_to_name(ret));
        spi_bus_free(SPI3_HOST);
        return ret;
    }
    ESP_LOGI(TAG_UC, "SPI init OK, handle=%p", spi_);
    return ESP_OK;
}

// ── 硬件复位 ─────────────────────────────────────────────────────────────────

void Uc8279cDisplay::hardwareReset() {
    // RST: HIGH 20ms → LOW 10ms → HIGH 100ms
    gpio_set_level(gn(EPD_RESET), 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(gn(EPD_RESET), 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(gn(EPD_RESET), 1);
    vTaskDelay(pdMS_TO_TICKS(100));
}

// ── 控制器初始化命令序列 ───────────────────────────────────────────────────

void Uc8279cDisplay::initController(bool grayMode) {
    const uint8_t panel[] = {0x3f, 0x4a};
    const uint8_t power[] = {static_cast<uint8_t>(grayMode ? 0x03 : 0x43), 0x00, 0x78, 0x78, 0x17};
    const uint8_t booster[] = {0x25, 0x25, 0x3c};
    const uint8_t resolution[] = {0x03, 0x00, 0x02, 0x58};  // 768×600
    const uint8_t interval[] = {0x00, 0x00, 0x20, 0x00};
    const uint8_t powerOffSequence = 0x20;
    const uint8_t vcom = grayMode ? 0x20 : 0x24;
    const uint8_t pll = 0x0f;
    const uint8_t tcon = 0x02;

    cmdData(CMD_PANEL_SETTING, panel, sizeof(panel));
    cmdData(CMD_POWER_OFF_SEQUENCE, &powerOffSequence, 1);
    cmdData(CMD_POWER_SETTING, power, sizeof(power));
    cmdData(CMD_BOOSTER_SOFT_START, booster, sizeof(booster));
    cmdData(CMD_VCOM_DC, &vcom, 1);
    cmdData(CMD_PLL_CONTROL, &pll, 1);
    cmdData(CMD_RESOLUTION, resolution, sizeof(resolution));
    cmdData(CMD_GATE_SOURCE_SETTING, interval, sizeof(interval));
    cmdData(CMD_TCON, &tcon, 1);

    if (grayMode) {
        // 灰阶模式：CrossLink 用较低驱动设置，先上电再填 DTM1
        sendCmd(CMD_POWER_ON);
        waitBusy("gray power-on");
        fillControllerRam(CMD_DTM1, 0xff);
    } else {
        // BW 模式：官方 1.2.7 先填 DTM1 再上电
        fillControllerRam(CMD_DTM1, 0xff);
        sendCmd(CMD_POWER_ON);
        waitBusy("power-on");
    }
    screenOn_ = true;
    grayControllerMode_ = grayMode;
}

void Uc8279cDisplay::ensurePowerOn() {
    if (screenOn_) return;
    sendCmd(CMD_POWER_ON);
    waitBusy("power-on");
    screenOn_ = true;
}

void Uc8279cDisplay::fillControllerRam(uint8_t cmd, uint8_t fill) {
    uint8_t row[PANEL_ROW_BYTES];
    memset(row, fill, sizeof(row));

    sendCmd(cmd);  // 命令用独立 CS 脉冲

    // 数据连续写: CS 保持 LOW
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    for (uint16_t y = 0; y < CONTROLLER_HEIGHT; ++y) {
        spiTransmit(row, PANEL_ROW_BYTES);
    }
    gpio_set_level(gn(EPD_CS), 1);
}

// ── LUT 加载 ─────────────────────────────────────────────────────────────────

void Uc8279cDisplay::loadFullLut() {
    const uint8_t* const lut[] = {
        A4_UC8279C_LUT_20, A4_UC8279C_LUT_21, A4_UC8279C_LUT_22,
        A4_UC8279C_LUT_23, A4_UC8279C_LUT_24};
    for (uint8_t i = 0; i < 5; ++i) {
        cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i), lut[i], A4_UC8279C_LUT_LENGTH);
    }
}

void Uc8279cDisplay::loadFastLut() {
    // CrossLink fast: 每条 49 字节记录仅写前 42 字节
    for (uint8_t i = 0; i < 5; ++i) {
        cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i),
                A4_UC8279C_FAST_LUT + i * A4_UC8279C_FAST_RECORD_LENGTH,
                A4_UC8279C_FAST_WRITE_LENGTH);
    }
}

void Uc8279cDisplay::loadGrayLut() {
    // 四级灰阶: 每条 42 字节记录
    for (uint8_t i = 0; i < 5; ++i) {
        cmdData(static_cast<uint8_t>(CMD_LUT_VCOM + i),
                A4_UC8279C_GRAY_LUT + i * A4_UC8279C_GRAY_RECORD_LENGTH,
                A4_UC8279C_GRAY_RECORD_LENGTH);
    }
}

// ── 帧写入 ───────────────────────────────────────────────────────────────────

void Uc8279cDisplay::writeFrame(uint8_t cmd, const uint8_t* fb) {
    uint8_t white[PANEL_ROW_BYTES];
    memset(white, 0xff, sizeof(white));

    sendCmd(cmd);  // 命令用独立 CS 脉冲

    // 数据连续写: CS 保持 LOW，从 height-1 行逆序到 0 行
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    for (int y = static_cast<int>(PANEL_HEIGHT) - 1; y >= 0; --y) {
        spiTransmit(fb + static_cast<uint32_t>(y) * PANEL_ROW_BYTES, PANEL_ROW_BYTES);
    }
    // 底部 48 行白填充 (控制器需要 600 行)
    for (uint16_t y = 0; y < CONTROLLER_PADDING_ROWS; ++y) {
        spiTransmit(white, PANEL_ROW_BYTES);
    }
    gpio_set_level(gn(EPD_CS), 1);
}

// ── 刷新 ─────────────────────────────────────────────────────────────────────

bool Uc8279cDisplay::refresh(bool turnOff) {
    sendCmd(CMD_DISPLAY_REFRESH);
    // 确认刷新已启动：BUSY 应在短窗口内拉低，否则刷新命令可能被控制器吞掉
    // （控制器状态异常时 REFRESH 不生效、BUSY 保持 HIGH，直接等待会误判为已完成）
    const int64_t t0 = esp_timer_get_time();
    bool started = false;
    while (esp_timer_get_time() - t0 < BUSY_START_CONFIRM_US) {
        if (gpio_get_level(gn(EPD_BUSY)) == 0) { started = true; break; }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    if (!started) {
        ESP_LOGW(TAG_UC, "refresh not started (BUSY stayed high), 命令可能被吞");
        return false;
    }
    waitBusy("refresh");
    if (turnOff) {
        sendCmd(CMD_POWER_OFF);
        waitBusy("power-off");
        screenOn_ = false;
    }
    return true;
}

void Uc8279cDisplay::display(RefreshMode mode, bool turnOff) {
    // 先等待上一帧刷新真正完成（避免打断正在进行的刷新）
    waitBusy("pre-wait");
    // 从灰阶模式恢复到 BW 模式：需重新初始化控制器（RST 脉冲，不断电）
    if (grayControllerMode_) {
        hardwareReset();
        initController(false);
    }
    ensurePowerOn();

    bool fast = (mode == RefreshMode::Fast);
    // Fast 限流: 连续 4 次后第 5 次升级为 Full
    if (fast && fastRefreshesSinceFull_ >= MAX_CONSECUTIVE_FAST) {
        fast = false;
    }
    if (fast) {
        ++fastRefreshesSinceFull_;
    } else {
        fastRefreshesSinceFull_ = 0;
    }

    const uint8_t interval = fast ? 0xd7 : 0x97;
    cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);

    if (fast) {
        loadFastLut();
    } else {
        // Half 和 Full 都用完整 LUT
        loadFullLut();
    }

    writeFrame(CMD_DTM2, fb_);
    if (!refresh(turnOff)) {
        // 刷新命令被吞：RST 重置控制器并重推一帧
        ESP_LOGW(TAG_UC, "display: 刷新未启动，重置控制器重试");
        waitBusy("retry-wait");
        hardwareReset();
        initController(false);
        ensurePowerOn();
        cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);
        if (fast) loadFastLut(); else loadFullLut();
        writeFrame(CMD_DTM2, fb_);
        if (!refresh(turnOff)) {
            ESP_LOGE(TAG_UC, "display: 重试仍失败，画面可能残留");
        }
    }
}

void Uc8279cDisplay::displayAsync(RefreshMode mode) {
    // 先等待上一帧刷新完成，再重置控制器（避免打断进行中的刷新）
    waitBusy("pre-wait");
    // 从灰阶模式恢复到 BW 模式：需重新初始化控制器
    if (grayControllerMode_) {
        hardwareReset();
        initController(false);
    }
    ensurePowerOn();

    bool fast = (mode == RefreshMode::Fast);
    if (fast && fastRefreshesSinceFull_ >= MAX_CONSECUTIVE_FAST) {
        fast = false;
    }
    if (fast) {
        ++fastRefreshesSinceFull_;
    } else {
        fastRefreshesSinceFull_ = 0;
    }

    const uint8_t interval = fast ? 0xd7 : 0x97;
    cmdData(CMD_VCOM_DATA_INTERVAL, &interval, 1);

    if (fast) {
        loadFastLut();
    } else {
        loadFullLut();
    }

    writeFrame(CMD_DTM2, fb_);
    sendCmd(CMD_DISPLAY_REFRESH);
    // 不等待 BUSY — 非阻塞
}

void Uc8279cDisplay::waitRefreshComplete() {
    waitBusy("refresh");
}

// ── 四级灰阶刷新 ─────────────────────────────────────────────────────────────

void Uc8279cDisplay::displayGray(bool turnOff, bool async) {
    // 未分配灰阶平面（无灰度内容）时直接返回，保留当前 BW 画面
    if (!fbGrayMsb_ || !fbGrayLsb_) return;

    // 先等待上一帧刷新完成（避免打断正在进行的刷新）
    waitBusy("gray-pre-wait");
    // 灰阶模式需要重新初始化控制器（grayMode=true，RST 脉冲，不断电）
    hardwareReset();
    initController(true);

    // CrossLink: MSB→DTM1, LSB→DTM2，均自底向上并补 48 行白填充
    writeFrame(CMD_DTM1, fbGrayMsb_);
    writeFrame(CMD_DTM2, fbGrayLsb_);
    loadGrayLut();

    if (async) {
        // 仅发起刷新，不等待 BUSY；turnOff 在异步路径下忽略
        sendCmd(CMD_DISPLAY_REFRESH);
    } else {
        if (!refresh(turnOff)) {
            // 刷新命令被吞：RST 重置控制器并重推一帧
            ESP_LOGW(TAG_UC, "displayGray: 刷新未启动，重置控制器重试");
            waitBusy("retry-wait");
            hardwareReset();
            initController(true);
            writeFrame(CMD_DTM1, fbGrayMsb_);
            writeFrame(CMD_DTM2, fbGrayLsb_);
            loadGrayLut();
            if (!refresh(turnOff)) {
                ESP_LOGE(TAG_UC, "displayGray: 重试仍失败，画面可能残留");
            }
        }
    }
    // 灰阶刷新后强制下一帧 BW 刷新用 Full LUT（升级判定从 4 次提前到立刻）：
    // 灰阶内容的残影需要用完整驱动波形才能彻底清除，
    // 否则翻到下一页（Fast LUT）时上一页图片内容会以"残影"残留
    fastRefreshesSinceFull_ = MAX_CONSECUTIVE_FAST;
}

// ── 深睡眠 ───────────────────────────────────────────────────────────────────

void Uc8279cDisplay::deepSleep() {
    fastRefreshesSinceFull_ = 0;
    if (screenOn_) {
        sendCmd(CMD_POWER_OFF);
        waitBusy("power-off");
        screenOn_ = false;
    }
    const uint8_t check = 0xa5;
    cmdData(CMD_DEEP_SLEEP, &check, 1);
}

// ── BUSY 等待 ────────────────────────────────────────────────────────────────

void Uc8279cDisplay::waitBusy(const char* tag) {
    (void)tag;
    const int64_t start = esp_timer_get_time();

    // 阶段1：等待进入忙态（初始即空闲时，最多等 settle 窗口确认）
    bool busy = (gpio_get_level(gn(EPD_BUSY)) == 0);
    if (!busy) {
        while (esp_timer_get_time() - start < BUSY_SETTLE_US) {
            if (gpio_get_level(gn(EPD_BUSY)) == 0) {
                busy = true;
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
    }

    // 阶段2：等待 BUSY 回 HIGH 且保持空闲确认时长，确保刷新真正结束。
    // 灰阶等刷新 BUSY 可能分段（短暂释放后又拉低），若只等第一次回 HIGH
    // 会提前返回，上层随后 hardwareReset 会打断仍在进行的刷新导致状态错乱。
    int idleMs = 0;
    while (esp_timer_get_time() - start < BUSY_TIMEOUT_US) {
        if (gpio_get_level(gn(EPD_BUSY)) == 0) {
            idleMs = 0;  // 又变忙：刷新未结束，重置空闲计数
        } else if (++idleMs >= BUSY_IDLE_CONFIRM_MS) {
            break;  // 持续空闲，确认完成
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

// ── SPI 底层传输 ─────────────────────────────────────────────────────────────

void Uc8279cDisplay::spiTransmit(const uint8_t* data, size_t len) {
    if (!spi_) {
        ESP_LOGE(TAG_UC, "spiTransmit: spi_ is NULL! len=%u", (unsigned)len);
        return;
    }
    spi_transaction_t t = {};
    t.length = static_cast<size_t>(len) * 8;  // 位长度
    t.tx_buffer = data;
    (void)spi_device_polling_transmit(spi_, &t);
}

void Uc8279cDisplay::sendCmd(uint8_t cmd) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 0);  // DC LOW = 命令
    spiTransmit(&cmd, 1);
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::sendData(uint8_t data) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);  // DC HIGH = 数据
    spiTransmit(&data, 1);
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::sendData(const uint8_t* data, size_t len) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 1);
    spiTransmit(data, len);
    gpio_set_level(gn(EPD_CS), 1);
}

void Uc8279cDisplay::cmdData(uint8_t cmd, const uint8_t* data, size_t len) {
    gpio_set_level(gn(EPD_CS), 0);
    gpio_set_level(gn(EPD_DC), 0);  // 命令
    spiTransmit(&cmd, 1);
    if (len > 0 && data != nullptr) {
        gpio_set_level(gn(EPD_DC), 1);  // 数据
        spiTransmit(data, len);
    }
    gpio_set_level(gn(EPD_CS), 1);
}

// ── Framebuffer 操作 (逻辑 portrait 坐标) ──────────────────────────────────

void Uc8279cDisplay::clear() {
    if (fb_) {
        memset(fb_, 0xff, FRAMEBUFFER_SIZE);  // 全白
    }
}

void Uc8279cDisplay::drawPixel(int16_t x, int16_t y, uint8_t color) {
    if (!fb_) return;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

    // portrait→native 旋转: nativeX=y, nativeY=551-x
    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);

    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);

    if (color) {
        fb_[byteIdx] |= (1 << bit);    // 1 = 白
    } else {
        fb_[byteIdx] &= ~(1 << bit);   // 0 = 黑
    }

    // 灰阶模式已激活时同步更新灰阶平面（黑→00, 白→11），保持 BW 与灰阶一致
    if (hasGray_) {
        drawPixelGray(x, y, color ? 15 : 0);
    }
}

uint8_t Uc8279cDisplay::getPixel(int16_t x, int16_t y) const {
    if (!fb_) return 1;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return 1;

    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);

    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);

    return (fb_[byteIdx] >> bit) & 1;
}

void Uc8279cDisplay::fillRect(int16_t x, int16_t y, int16_t w, int16_t h, uint8_t color) {
    if (!fb_) return;
    // 裁剪到有效范围
    if (w <= 0 || h <= 0) return;

    int16_t x0 = (x < 0) ? 0 : x;
    int16_t y0 = (y < 0) ? 0 : y;
    int16_t x1 = (x + w > WIDTH) ? WIDTH : (x + w);
    int16_t y1 = (y + h > HEIGHT) ? HEIGHT : (y + h);

    for (int16_t py = y0; py < y1; ++py) {
        for (int16_t px = x0; px < x1; ++px) {
            drawPixel(px, py, color);
        }
    }
}

// ── 四级灰阶 framebuffer 操作 (逻辑 portrait 坐标) ─────────────────────────

void Uc8279cDisplay::ensureGrayBuffers() {
    if (!fbGrayMsb_) {
        fbGrayMsb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (!fbGrayLsb_) {
        fbGrayLsb_ = static_cast<uint8_t*>(heap_caps_malloc(FRAMEBUFFER_SIZE, MALLOC_CAP_SPIRAM));
    }
    if (!hasGray_) {
        // 首次使用灰度：把 fb_ 同步到灰阶平面
        // fb_ bit=0(黑) → MSB=0,LSB=0(黑); fb_ bit=1(白) → MSB=1,LSB=1(白)
        if (fbGrayMsb_) memcpy(fbGrayMsb_, fb_, FRAMEBUFFER_SIZE);
        if (fbGrayLsb_) memcpy(fbGrayLsb_, fb_, FRAMEBUFFER_SIZE);
        hasGray_ = true;
    }
}

void Uc8279cDisplay::drawPixelGray(int16_t x, int16_t y, uint8_t gray) {
    if (!fb_) return;
    if (x < 0 || x >= WIDTH || y < 0 || y >= HEIGHT) return;

    ensureGrayBuffers();
    if (!fbGrayMsb_ || !fbGrayLsb_) return;  // PSRAM 分配失败，放弃灰阶

    if (gray > 15) gray = 15;
    // 16 级 → 4 级: 0-3=黑(00), 4-7=深灰(01), 8-11=浅灰(10), 12-15=白(11)
    const uint8_t val = gray >> 2;  // 0-3
    const uint8_t msb = (val >> 1) & 1;
    const uint8_t lsb = val & 1;

    // 与 drawPixel 相同的 portrait→native 旋转
    const uint16_t nativeX = static_cast<uint16_t>(y);
    const uint16_t nativeY = static_cast<uint16_t>(PANEL_HEIGHT - 1 - x);
    const uint32_t byteIdx = static_cast<uint32_t>(nativeY) * PANEL_ROW_BYTES + (nativeX >> 3);
    const uint8_t bit = 7 - (nativeX & 0x7);

    if (msb) {
        fbGrayMsb_[byteIdx] |= (1 << bit);
    } else {
        fbGrayMsb_[byteIdx] &= ~(1 << bit);
    }
    if (lsb) {
        fbGrayLsb_[byteIdx] |= (1 << bit);
    } else {
        fbGrayLsb_[byteIdx] &= ~(1 << bit);
    }
}

void Uc8279cDisplay::clearGray() {
    // 仅重置灰阶状态；缓冲内容在下次 drawPixelGray 时由 fb_ 重新同步
    hasGray_ = false;
}

}  // namespace eego
