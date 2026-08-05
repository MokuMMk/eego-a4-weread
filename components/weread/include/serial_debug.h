// serial_debug.h — 串口调试命令（Task 17）
//
// 后台任务监听 UART0，解析一行命令并分发到注册的回调。
// 支持的命令通过 registerCommand 注册；WereadApp 在 run() 中注册全部默认命令。
// 命令不区分大小写。命令格式：<cmd> [args]\n
//
// 纯 ESP-IDF，无 Arduino 依赖。代码在 namespace eego 中。
#pragma once

#include <cstdint>
#include <cstddef>

namespace eego {

class SerialDebug {
public:
    static SerialDebug& instance();

    // 初始化串口命令解析（启动后台任务监听 UART）
    void start();

    // 注册回调（访问 WereadApp 的方法）
    using CommandHandler = void (*)(const char* args);
    void registerCommand(const char* cmd, CommandHandler handler);

private:
    SerialDebug() = default;
    SerialDebug(const SerialDebug&) = delete;
    SerialDebug& operator=(const SerialDebug&) = delete;

    static void debugTask(void* arg);
    void processLine(const char* line);

    // 命令表（线性搜索，命令数少够用）
    struct CmdEntry {
        char cmd[16];
        CommandHandler handler;
    };
    static constexpr int MAX_COMMANDS = 32;
    CmdEntry commands_[MAX_COMMANDS] = {};
    int commandCount_ = 0;

    char lineBuf_[256];
    size_t linePos_ = 0;
    bool started_ = false;
};

}  // namespace eego
