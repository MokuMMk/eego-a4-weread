// EEGO A4 电源管理实现 — GPIO4 锁存 + 电池 ADC + 充电状态
// 纯 ESP-IDF v5.x，使用 adc_oneshot + adc_cali_scheme (curve_fitting)
#include "power_manager.h"
#include "frontlight.h"

#include <esp_log.h>
#include <esp_sleep.h>
#include <driver/i2c.h>
#include <esp_adc/adc_cali_scheme.h>
#include <cstring>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char* PM_TAG = "eego_pm";

namespace eego {

PowerManager& PowerManager::instance() {
    static PowerManager inst;
    return inst;
}

void PowerManager::holdPower() {
    // GPIO4 电源锁存 — 系统启动后首项硬件操作
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << POWER_LATCH);
    io_conf.mode = GPIO_MODE_OUTPUT;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    gpio_config(&io_conf);

    // 先解除 hold 才能驱动引脚
    gpio_hold_dis(static_cast<gpio_num_t>(POWER_LATCH));
    // 拉高锁存电源
    gpio_set_level(static_cast<gpio_num_t>(POWER_LATCH), 1);
    // 锁存引脚状态，保证轻睡/深睡期间不断电
    gpio_hold_en(static_cast<gpio_num_t>(POWER_LATCH));
    gpio_deep_sleep_hold_en();

    ESP_LOGI(PM_TAG, "Power latch engaged on GPIO%d", POWER_LATCH);
}

void PowerManager::initAdc() {
    if (adcUnit_) return;

    // GPIO10 = ADC1_CH9 (ESP32-S3)
    adc_oneshot_unit_init_cfg_t initCfg;
    memset(&initCfg, 0, sizeof(initCfg));
    initCfg.unit_id = ADC_UNIT_1;
    esp_err_t err = adc_oneshot_new_unit(&initCfg, &adcUnit_);
    if (err != ESP_OK) {
        ESP_LOGE(PM_TAG, "ADC unit init failed: %s", esp_err_to_name(err));
        return;
    }

    adc_oneshot_chan_cfg_t chanCfg;
    memset(&chanCfg, 0, sizeof(chanCfg));
    chanCfg.bitwidth = ADC_BITWIDTH_12;
    chanCfg.atten = ADC_ATTEN_DB_12;
    err = adc_oneshot_config_channel(adcUnit_, ADC_CHANNEL_9, &chanCfg);
    if (err != ESP_OK) {
        ESP_LOGE(PM_TAG, "ADC channel config failed: %s", esp_err_to_name(err));
    }

    // 校准 — ESP32-S3 仅支持 curve fitting
    adc_cali_curve_fitting_config_t caliCfg;
    memset(&caliCfg, 0, sizeof(caliCfg));
    caliCfg.unit_id = ADC_UNIT_1;
    caliCfg.chan = ADC_CHANNEL_9;
    caliCfg.atten = ADC_ATTEN_DB_12;
    caliCfg.bitwidth = ADC_BITWIDTH_12;
    err = adc_cali_create_scheme_curve_fitting(&caliCfg, &adcCali_);
    if (err != ESP_OK) {
        ESP_LOGW(PM_TAG, "ADC calibration failed (using raw): %s", esp_err_to_name(err));
        adcCali_ = nullptr;
    }
}

void PowerManager::detectLm3630a() {
    // 读取 FrontlightManager 缓存的前光存在性（首次探测由 FrontlightManager
    // 完成并缓存，此处非破坏性读取，不再 toggle GPIO12，避免关闭前光）
    lm3630aPresent_ = FrontlightManager::instance().present();

    // LM3630A 在位 → 充电状态 active-high（INPUT_PULLDOWN）
    // LM3630A 不在位 → 充电状态 active-low（INPUT_PULLUP）
    chargeActiveHigh_ = lm3630aPresent_;

    // 配置 GPIO11 充电状态引脚
    gpio_config_t io_conf;
    memset(&io_conf, 0, sizeof(io_conf));
    io_conf.pin_bit_mask = (1ULL << CHARGE_STATUS);
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    if (chargeActiveHigh_) {
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_ENABLE;
    } else {
        io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    }
    gpio_config(&io_conf);

    ESP_LOGI(PM_TAG, "LM3630A %s, charge status active-%s",
             lm3630aPresent_ ? "present" : "absent",
             chargeActiveHigh_ ? "high" : "low");
}

void PowerManager::ensureInitialized() {
    if (initialized_) return;
    initAdc();
    detectLm3630a();
    initialized_ = true;
}

uint8_t PowerManager::percentFromMillivolts(uint16_t mv) {
    // Li-ion 放电曲线（电压→百分比），线性插值
    // 3.00V→0%, 3.70V→20%, 3.80V→50%, 4.00V→80%, 4.10V→100%
    if (mv >= 4100) return 100;
    if (mv <= 3000) return 0;

    struct Point { uint16_t mv; uint8_t pct; };
    static constexpr Point points[] = {
        {3000, 0}, {3700, 20}, {3800, 50}, {4000, 80}, {4100, 100}
    };
    constexpr size_t N = sizeof(points) / sizeof(points[0]);

    for (size_t i = 1; i < N; i++) {
        if (mv <= points[i].mv) {
            const Point& p0 = points[i - 1];
            const Point& p1 = points[i];
            float frac = static_cast<float>(mv - p0.mv) /
                         static_cast<float>(p1.mv - p0.mv);
            float pct = p0.pct + frac * static_cast<float>(p1.pct - p0.pct);
            if (pct < 0.0f) pct = 0.0f;
            if (pct > 100.0f) pct = 100.0f;
            return static_cast<uint8_t>(pct + 0.5f);
        }
    }
    return 100;
}

BatteryInfo PowerManager::readBattery() {
    ensureInitialized();

    BatteryInfo info;
    info.millivolts = 0;
    info.percent = 0;
    info.charging = false;

    if (!adcUnit_) return info;

    // 采样多次取平均，降低噪声
    int rawSum = 0;
    const int samples = 4;
    for (int i = 0; i < samples; i++) {
        int raw = 0;
        if (adc_oneshot_read(adcUnit_, ADC_CHANNEL_9, &raw) != ESP_OK) {
            return info;
        }
        rawSum += raw;
    }
    int avgRaw = rawSum / samples;

    int adcMv = 0;
    if (adcCali_) {
        adc_cali_raw_to_voltage(adcCali_, avgRaw, &adcMv);
    } else {
        // 无校准时粗略换算：12bit @ 11dB 衰减 → 0~3.3V 范围
        adcMv = static_cast<int>(static_cast<float>(avgRaw) * 3300.0f / 4095.0f);
    }

    // 实际电池电压 = ADC 毫伏 × 分压系数
    info.millivolts = static_cast<uint16_t>(
        static_cast<float>(adcMv) * BATTERY_DIVIDER);
    info.percent = percentFromMillivolts(info.millivolts);
    info.charging = isCharging();

    ESP_LOGD(PM_TAG, "Battery: %umV → %u%%, raw=%d, adcMv=%d",
             info.millivolts, info.percent, avgRaw, adcMv);
    return info;
}

bool PowerManager::isCharging() {
    ensureInitialized();

    int level = gpio_get_level(static_cast<gpio_num_t>(CHARGE_STATUS));
    // active-high: 充电时 HIGH
    // active-low:  充电时 LOW
    return chargeActiveHigh_ ? (level == 1) : (level == 0);
}

}  // namespace eego
