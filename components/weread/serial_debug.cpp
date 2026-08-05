// serial_debug.cpp — 串口调试命令实现（Task 17）
//
// 后台任务通过 UART 驱动读取串口输入，逐字符积累一行，遇到 \r 或 \n 时
// 调用 processLine 解析命令并分发到注册的回调。
// UART0 是 ESP-IDF 默认控制台，uart_driver_install 可能返回 ESP_ERR_INVALID_STATE
// （已被控制台驱动安装），此时直接使用 uart_read_bytes 即可。
#include "serial_debug.h"
#include "weread_app.h"

#include "esp_log.h"
#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <cstring>
#include <cctype>

static const char* TAG = "eego.serial";

namespace eego {

static constexpr uart_port_t UART_NUM = UART_NUM_0;
static constexpr int UART_BUF_SIZE = 256;
static constexpr int TASK_STACK = 6144;
static constexpr UBaseType_t TASK_PRIORITY = 1;

SerialDebug& SerialDebug::instance() {
    static SerialDebug inst;
    return inst;
}

void SerialDebug::start() {
    if (started_) return;
    started_ = true;

    // 安装 UART 驱动（可能已被控制台安装，ESP_ERR_INVALID_STATE 可忽略）
    esp_err_t err = uart_driver_install(UART_NUM, UART_BUF_SIZE, UART_BUF_SIZE,
                                         0, nullptr, 0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
        started_ = false;
        return;
    }

    // 启动后台任务
    BaseType_t ret = xTaskCreate(debugTask, "serial_dbg", TASK_STACK,
                                  this, TASK_PRIORITY, nullptr);
    if (ret != pdPASS) {
        ESP_LOGE(TAG, "task creation failed");
        started_ = false;
    } else {
        ESP_LOGI(TAG, "串口调试命令已启动");
    }
}

void SerialDebug::registerCommand(const char* cmd, CommandHandler handler) {
    if (!cmd || !handler || commandCount_ >= MAX_COMMANDS) return;

    // 转小写存储
    CmdEntry& e = commands_[commandCount_++];
    size_t i = 0;
    for (; i < sizeof(e.cmd) - 1 && cmd[i]; i++) {
        e.cmd[i] = (char)tolower((unsigned char)cmd[i]);
    }
    e.cmd[i] = '\0';
    e.handler = handler;
}

void SerialDebug::debugTask(void* arg) {
    SerialDebug* self = static_cast<SerialDebug*>(arg);
    uint8_t ch;

    while (true) {
        // 从 UART0 读取一个字节，50ms 超时
        int len = uart_read_bytes(UART_NUM, &ch, 1, pdMS_TO_TICKS(50));
        if (len <= 0) continue;

        char c = (char)ch;
        if (c == '\r' || c == '\n') {
            if (self->linePos_ > 0) {
                self->lineBuf_[self->linePos_] = '\0';
                self->processLine(self->lineBuf_);
                self->linePos_ = 0;
            }
        } else if (self->linePos_ < sizeof(self->lineBuf_) - 1) {
            self->lineBuf_[self->linePos_++] = c;
        } else {
            // 行缓冲溢出，丢弃当前行
            self->linePos_ = 0;
        }
    }
}

void SerialDebug::processLine(const char* line) {
    if (!line || !line[0]) return;

    // 跳过前导空格
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    // 提取命令部分（到第一个空格或结尾）
    char cmd[16] = {};
    int ci = 0;
    const char* p = line;
    while (*p && *p != ' ' && *p != '\t' && ci < (int)sizeof(cmd) - 1) {
        cmd[ci++] = (char)tolower((unsigned char)*p);
        p++;
    }
    cmd[ci] = '\0';

    // 跳过命令与参数之间的空格
    while (*p == ' ' || *p == '\t') p++;
    const char* args = p;  // 可能为空字符串

    // 在注册的命令表中查找
    for (int i = 0; i < commandCount_; i++) {
        if (strcmp(cmd, commands_[i].cmd) == 0) {
            commands_[i].handler(args);
            return;
        }
    }

    // 未匹配注册命令：交给 WereadApp 处理（N:C / CFG {json} 等复杂格式）
    WereadApp::instance().handleSerialCommand(line);
}

}  // namespace eego
