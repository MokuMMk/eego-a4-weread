// weread_app.h — 微信读书阅读器应用主控制器（Task 14-15）
// 管理屏幕状态机、事件循环、所有 UI 屏幕渲染。
// 纯 ESP-IDF，无 Arduino 依赖。代码在 namespace eego 中。
#pragma once
#include "esp_err.h"
#include "uc8279c_display.h"
#include "gslx680_touch.h"
#include "buttons.h"
#include "portrait_canvas.h"
#include "font_manager.h"
#include "image_renderer.h"
#include "power_manager.h"
#include "config.h"
#include "storage.h"
#include "wifi_manager.h"
#include "weread_client.h"
#include "weread_api.h"
#include "weread_login.h"
#include "sleep_manager.h"
#include "serial_debug.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include <string>
#include <vector>
#include <cstdint>

namespace eego {

enum class ScreenId {
    Boot,
    Home,         // 主页（继续阅读卡片 + 书架入口 + 设置入口）
    Bookshelf,
    Detail,
    Reading,
    Toc,
    FontSelect,   // 版式 + 字体选择合并页（同原实现）
    Settings,     // 设置页
    Login,
    Provision
};

class WereadApp {
public:
    static WereadApp& instance();

    // 应用入口（从 app_main 调用）：初始化 → WiFi → 登录 → 书架 → 事件循环
    void run();

    // 屏幕切换
    void navigateTo(ScreenId screen);
    void goBack();

    // 刷新当前屏幕
    void refreshDisplay(RefreshMode mode = RefreshMode::Fast);

    // 输入处理
    void handleTouch(const TouchEvent& evt);
    void handleButton(const ButtonEvent& evt);

    // ---- 串口命令入口（由 SerialDebug 调用）----
    // 处理一行串口命令（不区分大小写）。未匹配注册命令时走此入口。
    void handleSerialCommand(const char* line);

    // ---- 休眠相关（由 SleepManager 调用）----
    // 浅睡前：显示当前书封面 + EPD 断电
    void displaySleepCover();
    // 唤醒后：恢复 EPD 电源 + 重绘当前页
    void onWakeFromSleep();
    // 触控驱动访问（SleepManager 检测双击用）
    Gslx680Touch* touch() { return touch_; }

    // ---- 串口命令需要的公开方法 ----
    void serialNextPage()   { turnPage(1); }
    void serialPrevPage()   { turnPage(-1); }
    void serialShowShelf()  { renderBookshelf(false); }
    void serialShelfNext()  { shelfPage_++; renderBookshelf(true); }
    void serialShelfPrev()  { shelfPage_--; renderBookshelf(true); }
    void serialShowToc();
    void serialTocNext();
    void serialTocPrev();
    void serialJumpChapter(const char* args);
    void serialOpenBook(const char* args);     // N:C 格式
    void serialDownloadAll();
    void serialStartProvisioning();
    void serialStartLogin();
    void serialPrintCookie();
    void serialPrintFonts();
    void serialPrintBooks();
    void serialUsbDiag();
    void serialToggleImgOff();
    void serialWriteConfig(const char* json);
    void serialPrintTime();
    void serialDeepSleep(const char* args);    // 深睡定时

private:
    WereadApp();
    WereadApp(const WereadApp&) = delete;
    WereadApp& operator=(const WereadApp&) = delete;

    // ---- 硬件实例 ----
    Uc8279cDisplay* display_;
    Gslx680Touch*  touch_;
    PortraitCanvas* canvas_;

    // ---- 屏幕状态 ----
    ScreenId currentScreen_ = ScreenId::Boot;
    ScreenId previousScreen_ = ScreenId::Boot;

    // ---- 全局状态 ----
    bool wifiConnected_  = false;
    bool isLoggedIn_     = false;
    bool offlineMode_    = false;

    // ---- 设置页重置确认状态 ----
    bool settingsResetPending_ = false;
    int64_t settingsResetTime_ = 0;

    // ---- 主页彩蛋 ----
    int  homeEasterClicks_ = 0;   // 空白区域累计点击次数
    bool homeEasterShown_  = false;  // 彩蛋文本当前是否显示

    // ---- 详情页删除整本下载确认状态 ----
    bool detailDeletePending_ = false;

    // ---- 书架数据 ----
    std::vector<weread_api::BookEntry> shelf_;
    int shelfPage_ = 0;
    int bookIdx_   = -1;   // 当前书在 shelf_ 中的下标
    weread_api::BookEntry curBook_;

    // ---- 目录数据 ----
    std::vector<weread_api::ChapterEntry> chapters_;
    std::string bookFormat_;
    int tocPage_ = 0;

    // ---- 阅读数据 ----
    std::vector<weread_api::ContentBlock> blocks_;
    std::string chapterTitle_;
    int chapterIdx_ = 0;
    int page_       = 0;
    int fastCount_  = 0;

    // 分页游标：每页起始 {块下标, 块内字节偏移}
    struct Cursor { size_t blk; size_t off; };
    std::vector<Cursor> pages_;

    // ---- 预取下一章 ----
    struct PrefetchCache {
        bool valid = false;
        bool loading = false;
        int chapterIdx = -1;
        std::vector<weread_api::ContentBlock> blocks;
        std::string title;
        std::string failedErr;  // 预取失败错误（非空表示已失败，如会员章节 e_0）
        void clear() { valid = false; loading = false; blocks.clear(); title.clear(); failedErr.clear(); }
    };
    PrefetchCache prefetch_;
    SemaphoreHandle_t prefetchSemaphore_ = nullptr;  // 信号量触发预取
    TaskHandle_t prefetchTask_ = nullptr;

    void prefetchTaskFunc();        // FreeRTOS 后台任务
    void triggerPrefetch();         // 触发预取（翻页接近章末时调用）
    bool tryConsumePrefetch(int chIdx);  // 尝试命中预取缓存

    // ---- 详情数据 ----
    weread_api::BookDetail detail_;
    std::vector<weread_api::Review> reviews_;

    // ---- 版式设置 ----
    int lineSpacing_   = 1;  // 0=紧凑 1=标准 2=宽松
    int paraGapMode_   = 1;  // 0=无 1=行距/4 2=行距/2
    int paraIndent_    = 2;  // 0=无 1=1字符 2=2字符
    bool layoutDirty_  = false;
    std::vector<std::string> fontList_;
    std::string bodyFontPath_;

    // ---- 屏幕渲染 ----
    void renderBootScreen(const char* subtitle);
    void splashStatus(const char* text);
    void renderBookshelf(bool fast);
    void renderBookDetail();
    void renderReadingPage(RefreshMode mode, bool wait);
    void renderToc();
    void renderFontSelect();
    void renderHome();
    void renderSettings();
    void renderMessageScreen(const char* l1, const char* l2 = "", const char* l3 = "");

    // ---- 状态栏图标 ----
    int  drawStatusBar(int y);
    int  drawBatteryIcon(int16_t x, int16_t y);
    int  drawWifiIcon(int16_t x, int16_t y);
    int  drawPowerIcon(int16_t x, int16_t y);

    // ---- 排版辅助 ----
    int  uiLineHeight() const;
    int  readLineHeight() const;
    int  pageAreaHeight() const;
    int  pageMaxWidth() const;
    int  paraGap() const;
    int  paraIndentWidth() const;
    int  shelfRowHeight() const;
    size_t nextLineEnd(const std::string& text, size_t idx, int maxW, float wscale);
    int  drawParagraph(const char* text, int16_t x, int16_t y, int16_t maxW, int maxLines);
    void truncatePx(std::string& s, int maxW);
    void paginateCurrent();

    // ---- 阅读操作 ----
    void turnPage(int delta);
    void openChapter(int chIdx, int page = 0);
    void continueReading();
    bool fetchToc(const std::string& bookId);
    void openBookDetail(int shelfIdx);
    void openBookDirect(int shelfIdx, int chIdx1Based);  // 串口 N:C 直读
    void downloadAll();                                    // 下载整本
    void saveProgress();
    bool tryRestoreReading();

    // ---- 字体/版式 ----
    void loadFontList();
    void applyFont(int idx);
    void applyLayout();
    void backToReading();

    // ---- 登录/配网 ----
    bool doQrLogin();
    // 配网引导屏：扫码联网（WIFI 二维码）+ captive portal 自动弹配网页
    void renderProvisionGuide();
    // 启动配网门户并阻塞等待配网结果：成功后重启，失败/超时返回
    void startProvisioningAndWait();

    // ---- 无 SD 兜底显示（嵌入式英文字库，不依赖 SD）----
    void drawAsciiFallback(const char* text, int16_t x, int16_t y,
                           int scale, bool bold = false);
    void showNoSdScreen();  // SD 未挂载：英文提示 + 点按重启

    // ---- 睡眠 ----
    void enterSleep();  // 调用 SleepManager::enterLightSleep()

    // ---- 触控分发 ----
    void handleShelfTouch(int16_t x, int16_t y);
    void handleDetailTouch(int16_t x, int16_t y);
    void handleReadingTouch(int16_t x, int16_t y);
    void handleTocTouch(int16_t x, int16_t y);
    void handleFontTouch(int16_t x, int16_t y);
    void handleHomeTouch(int16_t x, int16_t y);
    void handleSettingsTouch(int16_t x, int16_t y);

    // ---- 事件循环 ----
    void eventLoop();

    // ---- 初始化 ----
    void initHardware();
    void initStorage();
    void initFonts();
    void initWifi();
    void initLogin();
    void initShelf();
    void registerSerialCommands();  // 注册全部串口调试命令回调

    // ---- 工具 ----
    void drawCapsule(int x, int y, int w, bool selected);
    void screenMsg(const char* l1, const char* l2 = "", const char* l3 = "");
    // 延时期间检测触控，被触控打断返回 true
    bool pollTouchDuringDelay(uint32_t ms);
    // 等待触控返回主页（超时后也返回主页，避免卡死）
    void waitTouchBackHome(uint32_t timeoutMs = 30000);
};

}  // namespace eego
