// 微信读书阅读器 - EEGO A4 (ESP-IDF) 主入口
// Task 14-15： WereadApp 应用框架入口
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "weread_app.h"
#include "power_manager.h"

extern "C" void app_main(void) {
    // 1. 电源锁存（首项操作！GPIO4 锁存保持系统供电）
    eego::PowerManager::instance().holdPower();

    // 2. 运行应用（初始化硬件 → 加载配置 → 连 WiFi → 检查登录 → 显示书架 → 事件循环）
    eego::WereadApp::instance().run();
}
