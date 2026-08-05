// weread_app.cpp — 微信读书阅读器应用主控制器（Task 14-15）
// 移植自 papers3-weread-ref/src/main.cpp（Arduino → 纯 ESP-IDF）
// 管理屏幕状态机、事件循环、所有 UI 屏幕渲染。
#include "weread_app.h"
#include "boot_logo.h"
#include "ascii_font.h"
#include "icons.h"
#include "provision_portal.h"
#include "sd_card.h"
#include "sleep_manager.h"
#include "serial_debug.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "esp_ota_ops.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "mbedtls/platform.h"

#include <algorithm>
#include <cstring>
#include <cmath>
#include <cctype>
#include <cstdlib>
#include <ctime>

// qrcodegen Ecc 枚举与 Arduino LOW/HIGH 宏不冲突（ESP-IDF 无此宏）
#include "qrcodegen.h"

static const char* TAG = "weread_app";

// ---- OTA 升级常量 ----
#define OTA_FIRMWARE_URL "https://flash.kever.ink/firmware/papers3-weread-eegoa4.bin"
#define OTA_NVS_NAMESPACE "ota_info"
#define OTA_NVS_KEY "fw_time"

// ---- 屏幕几何常量 ----
static constexpr int16_t SCR_W = 552;   // PortraitCanvas::WIDTH
static constexpr int16_t SCR_H = 768;   // PortraitCanvas::HEIGHT
static constexpr int16_t PAD = 28;       // 水平边距（安全区）
static constexpr int16_t BODY_MARGIN = 28;  // 正文版心边距
static constexpr uint8_t BLACK = 0;
static constexpr uint8_t WHITE = 1;

static constexpr int FULL_REFRESH_EVERY = 8;
static constexpr int SHELF_PER_PAGE = 6;   // 3列×2行网格
static constexpr int LAYOUT_ROW_H = 44;
static constexpr int FONT_ROW_H = 48;
static constexpr int FONT_LIST_Y0 = 96 + 3 * LAYOUT_ROW_H + 16;
static constexpr int TOC_LIST_Y0 = 80;
static constexpr int AUTO_SLEEP_MS = 5 * 60 * 1000;

// 灰底区域（#D9D9D9 用 Bayer 抖动 gray=11 实现）
static void fillGray(eego::PortraitCanvas* cv, int16_t x, int16_t y, int16_t w, int16_t h) {
    for (int16_t yy = y; yy < y + h; yy++) {
        for (int16_t xx = x; xx < x + w; xx++) {
            cv->drawPixelGray(xx, yy, 11);
        }
    }
}
// 圆角灰底（gray=11），避免方角超出圆角边框
static void fillGrayRound(eego::PortraitCanvas* cv, int16_t x, int16_t y, int16_t w, int16_t h, int16_t r) {
    cv->fillRoundRectGray(x, y, w, h, r, 11);
}

// 字体放在 SD 卡根目录（用户可自定义文件名）
// UI 字体优先 .eefont（HarmonyOS Sans SC），自动发现根目录首个可用字体
// 正文默认与 UI 相同，用户可在字体选择屏切换
// 字号说明：SD 上只有 12/14/16/18 四档，UI 用 12（最小档）避免界面字体过大
static const char* UI_FONT_PREFERRED = "HarmonyOS_Sans_SC_12.eefont";
static const char* BODY_FONT_PREFERRED = "HarmonyOS_Sans_SC_Regular_24.bin";

// 在 SD 卡根目录扫描首个 .eefont/.bin 字体文件
static bool discoverFirstFont(std::string& outPath, bool preferEefont) {
    if (!eego::SdCard::instance().ready()) return false;
    // 目录项数组 ~17KB，放堆上避免爆主任务栈
    eego::SdCard::DirEntry* entries =
        (eego::SdCard::DirEntry*)calloc(64, sizeof(eego::SdCard::DirEntry));
    if (!entries) return false;
    int n = eego::SdCard::instance().listDir("/", entries, 64);
    std::string best;
    for (int i = 0; i < n; i++) {
        if (entries[i].isDir) continue;
        std::string name = entries[i].name;
        if (name.rfind("._", 0) == 0) continue;  // 跳过 macOS 元数据
        bool isEefont = (name.find(".eefont") != std::string::npos);
        bool isBin = (name.find(".bin") != std::string::npos);
        if (!isEefont && !isBin) continue;
        if (preferEefont && isEefont) { best = "/" + name; break; }
        if (!preferEefont && isBin) { best = "/" + name; break; }
        if (best.empty()) best = "/" + name;  // 退而求其次
    }
    free(entries);
    if (best.empty()) return false;
    outPath = best;
    return true;
}

namespace eego {

// ================================================================
// 单例 + 构造
// ================================================================

WereadApp& WereadApp::instance() {
    static WereadApp inst;
    return inst;
}

WereadApp::WereadApp()
    : display_(nullptr), touch_(nullptr), canvas_(nullptr) {}

// ================================================================
// run() — 应用主入口
// ================================================================

void WereadApp::run() {
    ESP_LOGI(TAG, "=== 微信读书阅读器启动 ===");

    // 1. NVS 自愈
    {
        esp_err_t nerr = nvs_flash_init();
        ESP_LOGI(TAG, "nvs_flash_init() = %d (%s)", nerr, esp_err_to_name(nerr));
        if (nerr == ESP_ERR_NVS_NO_FREE_PAGES || nerr == ESP_ERR_NVS_NEW_VERSION_FOUND) {
            ESP_LOGW(TAG, "NVS 初始化失败(%d)，擦除重建", nerr);
            nvs_flash_erase();
            nerr = nvs_flash_init();
            ESP_LOGI(TAG, "nvs_flash_init() 重建后 = %d", nerr);
        }
        // 主线程 NVS 写入测试（验证 flash 写是否正常）
        ESP_LOGI(TAG, "=== NVS 写入测试开始 ===");
        nvs_handle_t h;
        nerr = nvs_open("boot_test", NVS_READWRITE, &h);
        ESP_LOGI(TAG, "nvs_open = %d (%s)", nerr, esp_err_to_name(nerr));
        if (nerr == ESP_OK) {
            nerr = nvs_set_i32(h, "boot_cnt", 1);
            ESP_LOGI(TAG, "nvs_set_i32 = %d", nerr);
            nerr = nvs_commit(h);
            ESP_LOGI(TAG, "nvs_commit = %d", nerr);
            nvs_close(h);
            ESP_LOGI(TAG, "=== NVS 写入测试成功 ===");
        } else {
            ESP_LOGE(TAG, "=== NVS 写入测试失败 ===");
        }
    }

    // 2. mbedTLS 内存分配走 PSRAM（DRAM 不足时 TLS 握手会失败）
    // 延迟到 WiFi 初始化之后设置（避免影响 WiFi/NVS 内部分配）
    // 将在 initWifi() 完成后设置

    // 2.5 OTA 分区诊断（验证 esp_ota_get_running_partition 是否正常）
    {
        const esp_partition_t *p = esp_ota_get_running_partition();
        ESP_LOGI(TAG, "=== OTA 诊断 ===");
        ESP_LOGI(TAG, "running_partition ptr = %p", p);
        if (p) {
            ESP_LOGI(TAG, "  label=%s type=%d subtype=%d address=0x%x size=0x%x",
                     p->label, p->type, p->subtype, p->address, p->size);
        } else {
            ESP_LOGE(TAG, "  esp_ota_get_running_partition 返回 NULL!");
        }
    }

    // 2.6 OTA: 确认当前固件有效（取消回滚）
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "OTA: 当前固件已标记为有效");

    // 3. 初始化硬件
    initHardware();

    // 4. 初始化存储
    initStorage();

    // 5. 加载字体
    initFonts();

    // 6. 首屏
    renderBootScreen(nullptr);

    // 7. WiFi
    initWifi();

    // 8. WiFi 初始化完成后，mbedTLS 走 PSRAM（HTTPS 请求时需要）
    mbedtls_platform_set_calloc_free(
        [](size_t n, size_t size) -> void* {
            // 小块分配走内部 RAM（避免 DMA/flash 操作兼容性问题）
            if (n * size <= 4096) {
                void* p = heap_caps_malloc(n * size, MALLOC_CAP_INTERNAL);
                if (p) { memset(p, 0, n * size); return p; }
            }
            void* p = heap_caps_malloc(n * size, MALLOC_CAP_SPIRAM);
            if (!p) p = heap_caps_malloc(n * size, MALLOC_CAP_INTERNAL);
            if (p) memset(p, 0, n * size);
            return p;
        },
        heap_caps_free);

    // 8. 登录
    if (!offlineMode_) {
        initLogin();
    }

    // 9. 书架
    initShelf();

    ESP_LOGI(TAG, "Free heap: %lu, Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    // 10. 恢复阅读 or 显示主页
    if (!tryRestoreReading()) {
        renderHome();
    }

    // 11. 初始化休眠管理器（RTC）
    SleepManager::instance().init();

    // 12. 初始化串口调试命令
    SerialDebug::instance().start();
    registerSerialCommands();

    // 13. 初始化后台预取任务（Task 10）
    prefetchSemaphore_ = xSemaphoreCreateBinary();
    if (prefetchSemaphore_) {
        xTaskCreatePinnedToCore(
            [](void* arg) { static_cast<WereadApp*>(arg)->prefetchTaskFunc(); },
            "prefetch", 8192, this, 5, &prefetchTask_, 0);  // CPU0, 优先级5
    }

    // 13b. 初始化后台 WiFi 自动重连任务（离线/掉线时周期尝试联网，不卡 UI）
    xTaskCreatePinnedToCore(
        [](void* arg) { static_cast<WereadApp*>(arg)->wifiReconnectTaskFunc(); },
        "wifi_recon", 4096, this, 4, &wifiReconnectTask_, 0);  // CPU0, 低优先级

    // 14. 事件循环
    eventLoop();
}

// ================================================================
// 初始化
// ================================================================

void WereadApp::initHardware() {
    // 显示
    display_ = new Uc8279cDisplay();
    esp_err_t dret = display_->begin();
    if (dret != ESP_OK) {
        ESP_LOGE(TAG, "显示初始化失败: %s — 跳过刷新", esp_err_to_name(dret));
    } else {
        display_->clear();
        display_->display(RefreshMode::Full);
        display_->waitRefreshComplete();
    }

    // 画布
    canvas_ = new PortraitCanvas(*display_);
    canvas_->resetClip();

    // 触控
    touch_ = new Gslx680Touch();
    touch_->begin(I2C_NUM_0);

    // 按键
    ButtonManager::instance().begin();

    ESP_LOGI(TAG, "硬件初始化完成");
}

void WereadApp::initStorage() {
    // SD 卡
    SdCard::instance().mount();
    if (SdCard::instance().ready()) {
        ESP_LOGI(TAG, "SD 卡挂载成功");
        Storage::instance().initDirectories();
    } else {
        ESP_LOGW(TAG, "SD 卡挂载失败，无字体/配置可用");
        // 无 SD 卡时中文字体无法加载，用内置英文字库提示并等待点按重启
        showNoSdScreen();
    }

    // 配置
    Config::instance().load();

    // 版式参数
    const AppConfig& cfg = Config::instance().get();
    if (cfg.lineHeight > 0) lineSpacing_ = cfg.lineHeight;
    if (cfg.paragraphSpacing >= 0) paraGapMode_ = cfg.paragraphSpacing;
    if (cfg.firstLineIndent >= 0) paraIndent_ = cfg.firstLineIndent;
}

void WereadApp::initFonts() {
    if (!SdCard::instance().ready()) {
        ESP_LOGW(TAG, "SD 未就绪，跳过字体加载");
        return;
    }

    // UI 字体（固定）
    // UI 字体：优先首选名 → 自动发现 .eefont → 自动发现任意字体
    std::string uiPath = std::string("/") + UI_FONT_PREFERRED;
    if (!FontManager::instance().loadUiFont(uiPath.c_str())) {
        ESP_LOGW(TAG, "首选 UI 字体不存在: %s，自动扫描根目录", uiPath.c_str());
        std::string discovered;
        if (discoverFirstFont(discovered, true)) {
            uiPath = discovered;
        }
    }
    if (FontManager::instance().loadUiFont(uiPath.c_str())) {
        ESP_LOGI(TAG, "UI 字体加载成功: %s", uiPath.c_str());
    } else {
        ESP_LOGE(TAG, "UI 字体加载失败: %s（根目录无 .eefont/.bin）", uiPath.c_str());
    }

    // 正文字体：配置指定 → 首选名 → 自动发现 .bin → 回退 UI 字体
    const AppConfig& cfg = Config::instance().get();
    std::string bodyPath = (strlen(cfg.fontPath) > 0) ? cfg.fontPath : (std::string("/") + BODY_FONT_PREFERRED);
    if (!FontManager::instance().loadBodyFont(bodyPath.c_str())) {
        ESP_LOGW(TAG, "首选正文字体不存在: %s，自动扫描根目录", bodyPath.c_str());
        std::string discovered;
        if (discoverFirstFont(discovered, false)) {
            bodyPath = discovered;
        } else {
            bodyPath = uiPath;  // 最终回退到 UI 字体
        }
    }
    if (FontManager::instance().loadBodyFont(bodyPath.c_str())) {
        bodyFontPath_ = bodyPath;
        ESP_LOGI(TAG, "正文字体: %s", bodyFontPath_.c_str());
    } else {
        bodyFontPath_ = uiPath;
        ESP_LOGW(TAG, "正文字体回退 UI 字体: %s", bodyFontPath_.c_str());
    }
}

void WereadApp::initWifi() {
    WifiManager::instance().init();

    const AppConfig& cfg = Config::instance().get();
    if (cfg.wifiCount == 0) {
        // 无 WiFi 配置 → 配网门户（扫码联网 + captive portal 自动弹配网页）
        renderProvisionGuide();
        ProvisionPortal::instance().start();
        // 配网门户启动后阻塞等待用户配置
        // 用户配网成功后设备会重启；这里简单等待
        for (;;) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (WifiManager::instance().isConnected()) {
                ESP_LOGI(TAG, "配网成功，重启");
                esp_restart();
            }
        }
    }

    splashStatus("正在连接 WiFi...");
    WifiState st = WifiManager::instance().connectMulti();
    wifiConnected_ = (st == WifiState::Connected);
    if (!wifiConnected_) {
        ESP_LOGW(TAG, "WiFi 连接失败，进入离线模式");
        offlineMode_ = true;
        screenMsg("WiFi 未连接", "显示已缓存的书", "点右上角图标重连");
        vTaskDelay(pdMS_TO_TICKS(1000));
    } else {
        offlineMode_ = false;
        ESP_LOGI(TAG, "WiFi 已连接: %s", WifiManager::instance().currentSsid);
    }
}

void WereadApp::initLogin() {
    // 检查 cookie
    WR.loadConfig();
    isLoggedIn_ = WR.hasValidCookie();

    if (!isLoggedIn_) {
        ESP_LOGI(TAG, "无登录态，进入扫码登录");
        if (!doQrLogin()) {
            screenMsg("登录未完成", "点按屏幕重启");
            // 等待点按
            for (;;) {
                TouchEvent evt = touch_->poll();
                if (evt.type == TouchEventType::Touch && evt.pressed) {
                    esp_restart();
                }
                vTaskDelay(pdMS_TO_TICKS(50));
            }
        }
        isLoggedIn_ = true;
    }

    // 主动续期（与参考固件一致：失败不强制重扫——wr_skey 仍有几小时有效期，
    // 真遇到 -2012 会在请求层自动续期/重试，续不动才需要重新扫码）
    if (WR.hasValidCookie()) {
        if (WR.tryRenew()) {
            ESP_LOGI(TAG, "令牌已续期");
        } else if (WR.rtDead()) {
            // wr_rt 续期被拒：先用现有 wr_skey 继续（书架等请求若真 -2012 会走重扫）
            ESP_LOGW(TAG, "wr_rt 续期被拒，先用现有 wr_skey 继续（真 -2012 时再重扫）");
        } else {
            ESP_LOGW(TAG, "续期失败（网络/临时），稍后自动重试");
        }
    }
}

void WereadApp::initShelf() {
    if (offlineMode_) {
        // 离线模式：从缓存加载书架
        // 204 条目 × 500B ≈ 100KB，必须放堆上（主任务栈仅 16~32KB，栈上会爆栈踩坏内存）
        std::vector<ShelfEntry> entries(SHELF_PER_PAGE * 34);
        int n = Storage::instance().loadShelfCache(entries.data(), (int)entries.size());
        for (int i = 0; i < n; i++) {
            weread_api::BookEntry b;
            b.bookId = entries[i].bookId;
            b.title = entries[i].title;
            b.author = entries[i].author;
            b.cover = entries[i].cover;
            b.format = entries[i].format;
            b.progress = entries[i].progress;
            shelf_.push_back(b);
        }
        ESP_LOGI(TAG, "离线书架: %d 本", (int)shelf_.size());
        return;
    }

    splashStatus("正在加载书架...");
    std::string err;
    if (!weread_api::getBookshelf(shelf_, err) || shelf_.empty()) {
        // 尝试续期后重拉
        if (err.find("-2012") != std::string::npos) {
            if (WR.tryRenew()) {
                err.clear();
                weread_api::getBookshelf(shelf_, err);
            }
        }
        // 仍然失败 → 尝试缓存
        if (shelf_.empty()) {
            ESP_LOGW(TAG, "书架获取失败: %s，尝试缓存", err.c_str());
            // 100KB 数组同样放堆上，避免爆主任务栈
            std::vector<ShelfEntry> entries(SHELF_PER_PAGE * 34);
            int n = Storage::instance().loadShelfCache(entries.data(), (int)entries.size());
            for (int i = 0; i < n; i++) {
                weread_api::BookEntry b;
                b.bookId = entries[i].bookId;
                b.title = entries[i].title;
                b.author = entries[i].author;
                b.cover = entries[i].cover;
                b.format = entries[i].format;
                b.progress = entries[i].progress;
                shelf_.push_back(b);
            }
            offlineMode_ = true;
            if (shelf_.empty()) {
                // 书架拉取失败且缓存为空：允许用户退出到离线空书架，不再死等重启
                screenMsg("书架获取失败", err.c_str(), "点按进入离线模式");
                for (;;) {
                    TouchEvent evt = touch_->poll();
                    if (evt.type == TouchEventType::Touch && evt.pressed) break;
                    vTaskDelay(pdMS_TO_TICKS(50));
                }
            }
        }
    }

    // 按最近阅读排序
    std::stable_sort(shelf_.begin(), shelf_.end(),
        [](const weread_api::BookEntry& a, const weread_api::BookEntry& b) {
            return a.readUpdateTime > b.readUpdateTime;
        });

    // 保存缓存
    if (!shelf_.empty()) {
        std::vector<ShelfEntry> entries;
        entries.reserve(shelf_.size());
        for (auto& b : shelf_) {
            ShelfEntry e = {};
            strncpy(e.bookId, b.bookId.c_str(), sizeof(e.bookId) - 1);
            strncpy(e.title, b.title.c_str(), sizeof(e.title) - 1);
            strncpy(e.author, b.author.c_str(), sizeof(e.author) - 1);
            strncpy(e.cover, b.cover.c_str(), sizeof(e.cover) - 1);
            strncpy(e.format, b.format.c_str(), sizeof(e.format) - 1);
            e.progress = b.progress;
            entries.push_back(e);
        }
        Storage::instance().saveShelfCache(entries.data(), entries.size());
    }

    ESP_LOGI(TAG, "书架: %d 本", (int)shelf_.size());
}

// ================================================================
// 首屏
// ================================================================

void WereadApp::renderBootScreen(const char* subtitle) {
    canvas_->clear();

    // 图标 160×160（居中偏上）
    int lx = (SCR_W - BOOT_LOGO_W) / 2;
    int ly = 220;
    for (int yy = 0; yy < BOOT_LOGO_H; yy++) {
        for (int xx = 0; xx < BOOT_LOGO_W; xx++) {
            uint8_t v = BOOT_LOGO[yy * BOOT_LOGO_W + xx];
            canvas_->drawPixelGray(lx + xx, ly + yy, v * 15 / 255);
        }
    }

    // 标题
    if (FontManager::instance().uiLoaded()) {
        const char* title = "微信读书";
        int tw = FontManager::instance().uiTextWidth(title);
        FontManager::instance().drawText(*canvas_, (SCR_W - tw) / 2, ly + BOOT_LOGO_H + 56, title, 1, true);
        if (subtitle) {
            int sw = FontManager::instance().uiTextWidth(subtitle);
            FontManager::instance().drawText(*canvas_, (SCR_W - sw) / 2, ly + BOOT_LOGO_H + 136, subtitle, 1, false);
        }
    }

    // 技术信息（串口）
    ESP_LOGI(TAG, "Free heap: %lu, Free PSRAM: %lu",
             (unsigned long)esp_get_free_heap_size(),
             (unsigned long)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));

    canvas_->display(RefreshMode::Full);
    currentScreen_ = ScreenId::Boot;
}

void WereadApp::splashStatus(const char* text) {
    // 只清状态区（底部 80px）
    canvas_->fillRect(0, SCR_H - 100, SCR_W, 100, WHITE);
    if (FontManager::instance().uiLoaded()) {
        int tw = FontManager::instance().uiTextWidth(text);
        FontManager::instance().drawText(*canvas_, (SCR_W - tw) / 2, SCR_H - 70, text, 1, false);
    }
    canvas_->display(RefreshMode::Fast);
}

void WereadApp::renderMessageScreen(const char* l1, const char* l2, const char* l3) {
    screenMsg(l1, l2, l3);
}

void WereadApp::screenMsg(const char* l1, const char* l2, const char* l3) {
    canvas_->clear();
    if (FontManager::instance().uiLoaded()) {
        FontManager::instance().drawText(*canvas_, PAD, 60, l1, 1, false);
        if (l2 && l2[0]) FontManager::instance().drawText(*canvas_, PAD, 150, l2, 1, false);
        if (l3 && l3[0]) FontManager::instance().drawText(*canvas_, PAD, 240, l3, 1, false);
    }
    canvas_->display(RefreshMode::Full);
}

// 可取消加载页：显示提示 + 底部"点按退出"小字，轮询触控/按键
// 返回 true 表示用户取消，false 表示超时未取消
bool WereadApp::screenMsgCancelable(const char* l1, const char* l2, const char* l3,
                                    uint32_t timeoutMs) {
    canvas_->clear();
    if (FontManager::instance().uiLoaded()) {
        FontManager::instance().drawText(*canvas_, PAD, 60, l1, 1, false);
        if (l2 && l2[0]) FontManager::instance().drawText(*canvas_, PAD, 150, l2, 1, false);
        if (l3 && l3[0]) FontManager::instance().drawText(*canvas_, PAD, 240, l3, 1, false);
        // 底部小字提示
        FontManager::instance().drawText(*canvas_, PAD, SCR_H - 40, "点按退出", 0, false);
    }
    canvas_->display(RefreshMode::Full);

    // 轮询触控/按键，超时返回 false
    uint32_t elapsed = 0;
    const uint32_t step = 50;
    while (elapsed < timeoutMs) {
        TouchEvent evt = touch_->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            return true;
        }
        ButtonEvent bevt;
        if (ButtonManager::instance().poll(&bevt, 0)) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return false;
}

// 延时期间检测触控，被触控打断返回 true
bool WereadApp::pollTouchDuringDelay(uint32_t ms) {
    uint32_t elapsed = 0;
    const uint32_t step = 50;
    while (elapsed < ms) {
        TouchEvent evt = touch_->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    return false;
}

// 等待触控返回主页（超时后也返回主页，避免卡死）
void WereadApp::waitTouchBackHome(uint32_t timeoutMs) {
    uint32_t elapsed = 0;
    const uint32_t step = 50;
    while (elapsed < timeoutMs) {
        TouchEvent evt = touch_->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            renderHome();
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    // 超时也返回主页
    renderHome();
}

// ================================================================
// 状态栏图标
// ================================================================

int WereadApp::drawBatteryIcon(int16_t x, int16_t y) {
    const int W = 36, H = 20;
    // 外框
    canvas_->drawRect(x, y, W - 4, H, BLACK);
    canvas_->drawRect(x + 1, y + 1, W - 6, H - 2, BLACK);
    canvas_->fillRect(x + W - 4, y + 5, 4, H - 10, BLACK);  // 正极凸头

    BatteryInfo bat = PowerManager::instance().readBattery();
    int pct = bat.percent;
    if (pct > 100) pct = 100;
    int segs = (pct > 66) ? 3 : (pct > 33) ? 2 : (pct > 5) ? 1 : 0;

    int segW = 8, segGap = 2, innerX = x + 4, innerY = y + 4, innerH = H - 8;
    for (int i = 0; i < segs; i++) {
        int sx = innerX + i * (segW + segGap);
        canvas_->fillRect(sx, innerY, segW, innerH, BLACK);
    }

    // 充电闪电
    if (bat.charging && segs > 0) {
        int cx = x + W / 2 - 2, cy = y + 3;
        canvas_->drawLine(cx + 3, cy, cx, cy + 7, WHITE);
        canvas_->drawLine(cx, cy + 7, cx + 3, cy + 7, WHITE);
        canvas_->drawLine(cx + 3, cy + 7, cx - 1, cy + 14, WHITE);
    }
    return W;
}

int WereadApp::drawWifiIcon(int16_t x, int16_t y) {
    const int W = 28, H = 18;
    if (!WifiManager::instance().isConnected()) {
        // 断开：画 X
        for (int i = 0; i < 2; i++) {
            canvas_->drawLine(x + 4 + i, y + 4, x + W - 4 + i, y + H - 4, BLACK);
            canvas_->drawLine(x + W - 4 - i, y + 4, x + 4 - i, y + H - 4, BLACK);
        }
        return W;
    }
    int rssi = WifiManager::instance().rssi();
    int bars = (rssi > -55) ? 3 : (rssi > -70) ? 2 : 1;
    int cx = x + W / 2, cy = y + H - 2;
    for (int i = 0; i < bars; i++) {
        int r = 6 + i * 7;
        for (int a = -60; a <= 60; a += 8) {
            float rad = a * M_PI / 180.0f;
            int px = cx + (int)(r * sin(rad));
            int py = cy - (int)(r * cos(rad));
            canvas_->fillCircle(px, py, 2, BLACK);
        }
    }
    canvas_->fillCircle(cx, cy - 1, 3, BLACK);
    return W;
}

int WereadApp::drawPowerIcon(int16_t x, int16_t y) {
    const int W = 24, H = 20;
    int cx = x + W / 2, cy = y + H / 2 + 1;
    // 圆形（顶部留缺口）
    for (int a = -50; a <= 230; a += 8) {
        float rad = a * M_PI / 180.0f;
        int px = cx + (int)(9 * cos(rad));
        int py = cy + (int)(9 * sin(rad));
        canvas_->fillCircle(px, py, 2, BLACK);
    }
    // 竖线
    canvas_->fillRect(cx - 2, y, 5, 10, BLACK);
    return W;
}

int WereadApp::drawStatusBar(int y) {
    // 简化：电量纯数字百分比 + WiFi图标（不画电池图标和电源图标）
    auto& fm = FontManager::instance();
    BatteryInfo bat = PowerManager::instance().readBattery();
    char batText[16];
    snprintf(batText, sizeof(batText), "%d%%", (int)bat.percent);

    // WiFi 图标（左侧）
    drawWifiIcon(SCR_W - PAD - 60, y + 2);

    // 电量数字（右侧）
    int bw = fm.uiTextWidth(batText);
    fm.drawText(*canvas_, SCR_W - PAD - bw, y, batText, 1, false);

    return SCR_W - PAD - bw - 40;
}

// ================================================================
// 排版辅助
// ================================================================

int WereadApp::uiLineHeight() const {
    int h = FontManager::instance().uiFontHeight();
    return (h > 0 ? h : 22) + 10;
}

int WereadApp::readLineHeight() const {
    int h = FontManager::instance().bodyFontHeight();
    int base = (h > 0 ? h : 22);
    int gap = (lineSpacing_ == 0) ? 6 : (lineSpacing_ == 2) ? 16 : 10;
    return base + gap;
}

int WereadApp::pageAreaHeight() const {
    return SCR_H - 80 - 40;  // 标题 80 + 页脚 40
}

int WereadApp::pageMaxWidth() const {
    return SCR_W - 2 * BODY_MARGIN;
}

int WereadApp::paraGap() const {
    if (paraGapMode_ == 0) return 0;
    return readLineHeight() / ((paraGapMode_ == 2) ? 2 : 4);
}

int WereadApp::paraIndentWidth() const {
    if (paraIndent_ == 0) return 0;
    int cw = FontManager::instance().bodyTextWidth("一");
    if (cw <= 0) cw = 18;
    return cw * paraIndent_;
}

int WereadApp::shelfRowHeight() const {
    return (SCR_H - 140) / SHELF_PER_PAGE;
}

size_t WereadApp::nextLineEnd(const std::string& text, size_t idx, int maxW, float wscale) {
    size_t len = text.length();
    size_t end = idx;
    int w = 0;
    while (end < len) {
        uint8_t c = (uint8_t)text[end];
        if (c == '\n') { end++; break; }
        size_t step = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        if (end + step > len) break;
        char buf[5];
        memcpy(buf, text.c_str() + end, step);
        buf[step] = '\0';
        int cw = (int)(FontManager::instance().bodyTextWidth(buf) * wscale);
        if (w + cw > maxW && end > idx) break;
        w += cw;
        end += step;
    }
    return (end == idx) ? idx + 1 : end;
}

int WereadApp::drawParagraph(const char* text, int16_t x, int16_t y, int16_t maxW, int maxLines) {
    std::string s(text);
    int lines = 0;
    size_t idx = 0;
    int lh = uiLineHeight();
    auto& fm = FontManager::instance();
    while (idx < s.length() && lines < maxLines) {
        // 用 UI 字体测宽折行
        size_t len = s.length();
        size_t end = idx;
        int w = 0;
        while (end < len) {
            uint8_t c = (uint8_t)s[end];
            if (c == '\n') { end++; break; }
            size_t step = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
            if (end + step > len) break;
            char buf[5];
            memcpy(buf, s.c_str() + end, step);
            buf[step] = '\0';
            int cw = fm.uiTextWidth(buf);
            if (w + cw > maxW && end > idx) break;
            w += cw;
            end += step;
        }
        if (end == idx) end = idx + 1;

        std::string line = s.substr(idx, end - idx);
        // trim
        size_t start = line.find_first_not_of(" \t\r\n");
        if (start != std::string::npos) {
            size_t finish = line.find_last_not_of(" \t\r\n");
            line = line.substr(start, finish - start + 1);
        } else {
            line = "";
        }
        if (!line.empty()) {
            fm.drawText(*canvas_, x, y, line.c_str(), 1, false);
            y += lh;
            lines++;
        }
        idx = end;
    }
    return y;
}

void WereadApp::truncatePx(std::string& s, int maxW) {
    auto& fm = FontManager::instance();
    if (fm.uiTextWidth(s.c_str()) <= maxW) return;
    std::string out;
    size_t i = 0;
    while (i < s.length()) {
        uint8_t c = (uint8_t)s[i];
        size_t step = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
        if (i + step > s.length()) break;
        std::string ch = s.substr(i, step);
        std::string test = out + ch + "…";
        if (fm.uiTextWidth(test.c_str()) > maxW) break;
        out += ch;
        i += step;
    }
    s = out + "…";
}

// ================================================================
// 主页 Home
// ================================================================

void WereadApp::renderHome() {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::Home;
    canvas_->clear();

    auto& fm = FontManager::instance();

    // 1. 顶部电量（纯数字百分比，16px 置灰，右上角）
    BatteryInfo bat = PowerManager::instance().readBattery();
    char batText[16];
    snprintf(batText, sizeof(batText), "%d%%", (int)bat.percent);
    canvas_->drawTextCentered(444, 32, 80, batText, 1, false);

    // 2. 继续阅读卡片：灰底圆角 24（无边框）
    // 卡片区域 x=28 y=64 w=496 h=216
    fillGrayRound(canvas_, 28, 64, 496, 216, 24);

    if (curBook_.bookId.empty()) {
        // 无阅读记录：卡片中央显示"暂无阅读记录"
        canvas_->drawTextCentered(28, 158, 496, "暂无阅读记录", 1, false);
    } else {
        // 2a. 封面缩略图 x=52 y=104 w=88 h=120 圆角8 白底（无边框）
        bool coverDrawn = false;
        if (!curBook_.cover.empty() && SdCard::instance().ready()) {
            char thumbPath[256];
            if (ImageRenderer::instance().getThumbnail(curBook_.cover.c_str(), thumbPath, sizeof(thumbPath))) {
                coverDrawn = ImageRenderer::instance().renderFromFile(*canvas_, thumbPath, 52, 104, 88, 120);
            }
        }
        if (!coverDrawn) {
            canvas_->fillRoundRect(52, 104, 88, 120, 8, WHITE);
        }

        // 2b. 书名 x=160 y=96 w=340 scale=1 加粗 单行截断
        std::string title = curBook_.title;
        truncatePx(title, 340);
        fm.drawText(*canvas_, 160, 100, title.c_str(), 1, true);

        // 2c. 进度文案 x=160 y=146 "阅读进度 N%"
        // 优先显示本地真实进度（progress.json 中保存的 percent），无本地记录时兜底服务器进度
        int shownPct = curBook_.progress;
        if (SdCard::instance().ready()) {
            BookProgress bp;
            if (Storage::instance().loadProgress(curBook_.bookId.c_str(), bp) && bp.percent >= 0) {
                shownPct = bp.percent;
            }
        }
        char progText[64];
        snprintf(progText, sizeof(progText), "阅读进度 %d%%", shownPct);
        fm.drawText(*canvas_, 160, 146, progText, 1, false);

        // 2d/2e. 进度条 x=160 y=182 w=320 h=14 圆角7（无边框）
        fillGrayRound(canvas_, 160, 182, 320, 14, 7);
        int fillW = 320 * shownPct / 100;
        if (fillW > 0) {
            canvas_->fillRoundRect(160, 182, fillW, 14, 7, BLACK);
        }

        // 2f. 上次读到 x=160 y=216（本地进度优先，无本地记录时用服务器进度兜底）
        std::string lastRead = "上次读到 ";
        int locCh = -1;
        if (!curBook_.bookId.empty() && SdCard::instance().ready()) {
            BookProgress bp;
            if (Storage::instance().loadProgress(curBook_.bookId.c_str(), bp)) {
                locCh = bp.chapterIdx;
            }
        }
        if (!chapters_.empty() && locCh >= 0 && locCh < (int)chapters_.size()) {
            lastRead += "第" + std::to_string(locCh + 1) + "章·" + chapters_[locCh].title;
        } else if (!chapters_.empty() && !curBook_.progressChapterUid.empty()) {
            for (size_t i = 0; i < chapters_.size(); i++) {
                if (chapters_[i].chapterUid == curBook_.progressChapterUid) {
                    lastRead += "第" + std::to_string(i + 1) + "章·" + chapters_[i].title;
                    break;
                }
            }
        }
        if (lastRead == "上次读到 ") {
            lastRead += curBook_.title;
        }
        truncatePx(lastRead, 340);
        fm.drawText(*canvas_, 160, 216, lastRead.c_str(), 1, false);
    }

    // 3. 书架区块标题 x=28 y=312 "书架"
    fm.drawText(*canvas_, 28, 312, "书架", 1, true);

    // 4/5. 书封1 x=28 y=356 w=140 h=190, 书封2 x=184 y=356 w=140 h=190
    for (int i = 0; i < 2 && i < (int)shelf_.size(); i++) {
        int bx = (i == 0) ? 28 : 184;
        int by = 356;
        auto& b = shelf_[i];

        canvas_->fillRoundRect(bx, by, 140, 190, 12, WHITE);

        bool drew = false;
        if (!b.cover.empty() && SdCard::instance().ready()) {
            char thumbPath[256];
            if (ImageRenderer::instance().getThumbnail(b.cover.c_str(), thumbPath, sizeof(thumbPath))) {
                drew = ImageRenderer::instance().renderFromFile(*canvas_, thumbPath, bx + 4, by + 4, 132, 182);
            }
        }
        if (!drew) {
            std::string t = b.title;
            truncatePx(t, 120);
            int tw = fm.uiTextWidth(t.c_str());
            fm.drawText(*canvas_, bx + (140 - tw) / 2, by + 90, t.c_str(), 1, false);
        }
    }

    // 6. 设置热区+齿轮icon x=402 y=398 w=72 h=72
    canvas_->drawIcon(402, 398, ICON_SETTINGS_72, 72, 72, BLACK);

    // 7. 底部提示 x=166 y=712 "按电源键关机"
    canvas_->drawTextCentered(166, 712, 220, "按电源键关机", 1, false);

    // 8. 彩蛋：空白区点击 10 次后显示（再点一次消失）
    // 显示在书架（书封 y=356~546）下方的空白处，靠左对齐
    if (homeEasterShown_) {
        const char* lines[3] = {
            "On my business card, I am a corporate president.",
            "In my mind, I am a game developer.",
            "But in my heart, I am a gamer",
        };
        for (int i = 0; i < 3; i++) {
            fm.drawText(*canvas_, PAD, 560 + i * 30, lines[i], 1, false);
        }
    }

    canvas_->display(RefreshMode::Full);
}

// ================================================================
// 书架页
// ================================================================

void WereadApp::renderBookshelf(bool fast) {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::Bookshelf;
    canvas_->clear();

    auto& fm = FontManager::instance();

    // 1. 返回箭头（左上角）
    canvas_->drawIcon(36, 32, ICON_CHEVRON_LEFT_32, 32, 32, BLACK);

    // 2. 标题 居中 scale=1 加粗
    canvas_->drawTextCentered(216, 28, 120, "书架", 1, true);

    if (offlineMode_) {
        fm.drawText(*canvas_, PAD, 80, "离线", 1, false);
    }

    // 3. 3列网格布局，每本 149×185 圆角8
    static const int16_t COL_X[3] = {28, 201, 374};
    static const int16_t ROW_Y[2] = {96, 357};
    static const int16_t COVER_W = 149;
    static const int16_t COVER_H = 185;

    int totalPages = std::max(1, ((int)shelf_.size() + SHELF_PER_PAGE - 1) / SHELF_PER_PAGE);
    if (shelfPage_ < 0) shelfPage_ = 0;
    if (shelfPage_ >= totalPages) shelfPage_ = totalPages - 1;

    int start = shelfPage_ * SHELF_PER_PAGE;
    int end = std::min(start + SHELF_PER_PAGE, (int)shelf_.size());

    for (int i = start; i < end; i++) {
        int slot = i - start;
        int col = slot % 3;
        int row = slot / 3;
        int16_t bx = COL_X[col];
        int16_t by = ROW_Y[row];
        auto& b = shelf_[i];

        bool selected = (i == bookIdx_);

        // 封面框（无边框，选中态用灰底区分）
        canvas_->fillRoundRect(bx, by, COVER_W, COVER_H, 8, WHITE);
        if (selected) {
            // 选中态：画一圈灰底边框
            fillGrayRound(canvas_, bx - 4, by - 4, COVER_W + 8, COVER_H + 8, 12);
            canvas_->fillRoundRect(bx, by, COVER_W, COVER_H, 8, WHITE);
        }

        // 封面图
        bool drew = false;
        if (!b.cover.empty() && SdCard::instance().ready()) {
            char thumbPath[256];
            if (ImageRenderer::instance().getThumbnail(b.cover.c_str(), thumbPath, sizeof(thumbPath))) {
                drew = ImageRenderer::instance().renderFromFile(*canvas_, thumbPath,
                                                                 bx + 4, by + 4, COVER_W - 8, COVER_H - 8);
            }
        }
        if (!drew) {
            std::string t = b.title;
            truncatePx(t, COVER_W - 16);
            int tw = fm.uiTextWidth(t.c_str());
            fm.drawText(*canvas_, bx + (COVER_W - tw) / 2, by + COVER_H / 2 - 8, t.c_str(), 1, false);
        }

        // 书名（封面下方，两行截断）
        int nameY = by + COVER_H + 4;
        std::string name = b.title;
        // 简单两行截断：按宽度切分
        int maxW = COVER_W;
        std::string line1, line2;
        size_t idx = 0;
        int w1 = 0;
        while (idx < name.length()) {
            uint8_t c = (uint8_t)name[idx];
            size_t step = (c < 0x80) ? 1 : (c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4));
            if (idx + step > name.length()) break;
            std::string ch = name.substr(idx, step);
            int cw = fm.uiTextWidth(ch.c_str());
            if (w1 + cw > maxW && !line1.empty()) {
                // 剩余放入 line2
                line2 = name.substr(idx);
                break;
            }
            line1 += ch;
            w1 += cw;
            idx += step;
        }
        if (!line1.empty()) {
            fm.drawText(*canvas_, bx, nameY, line1.c_str(), 1, false);
        }
        if (!line2.empty()) {
            truncatePx(line2, maxW);
            fm.drawText(*canvas_, bx, nameY + 20, line2.c_str(), 1, false);
        }
    }

    // 4. 底部计数 居中
    char cnt[32];
    snprintf(cnt, sizeof(cnt), "共 %d 本", (int)shelf_.size());
    canvas_->drawTextCentered(226, 704, 100, cnt, 1, false);

    if (fast) {
        fastCount_++;
        if (fastCount_ % FULL_REFRESH_EVERY == 0) {
            canvas_->display(RefreshMode::Full);
        } else {
            canvas_->display(RefreshMode::Fast);
        }
    } else {
        canvas_->display(RefreshMode::Full);
        fastCount_ = 0;
    }
}

// ================================================================
// 书籍详情页
// ================================================================

void WereadApp::renderBookDetail() {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::Detail;
    canvas_->clear();

    auto& fm = FontManager::instance();
    auto& b = curBook_;
    std::string title = detail_.title.empty() ? b.title : detail_.title;
    std::string author = detail_.author.empty() ? b.author : detail_.author;

    fm.drawText(*canvas_, PAD, 16, "书籍详情", 1, false);
    drawStatusBar(14);

    // 封面
    bool coverDrawn = false;
    if (!b.cover.empty() && SdCard::instance().ready()) {
        char thumbPath[256];
        if (ImageRenderer::instance().getThumbnail(b.cover.c_str(), thumbPath, sizeof(thumbPath))) {
            coverDrawn = ImageRenderer::instance().renderFromFile(*canvas_, thumbPath, PAD, 70, 180, 250);
        }
    }
    if (!coverDrawn) {
        canvas_->drawRect(PAD, 70, 180, 250, BLACK);
        fm.drawText(*canvas_, PAD + 40, 190, "无封面", 1, false);
    }

    // 右上：标题/作者/出版社/进度
    int x0 = PAD + 200, y = 86;
    y = drawParagraph(title.c_str(), x0, y, SCR_W - x0 - PAD, 3) + 8;
    if (!author.empty()) {
        std::string a = author;
        truncatePx(a, 260);
        fm.drawText(*canvas_, x0, y, a.c_str(), 1, false);
        y += uiLineHeight();
    }
    if (!detail_.publisher.empty()) {
        std::string p = detail_.publisher;
        truncatePx(p, 260);
        fm.drawText(*canvas_, x0, y, p.c_str(), 1, false);
        y += uiLineHeight();
    }
    if (b.progress > 0) {
        char pg[32];
        snprintf(pg, sizeof(pg), "已读 %d%%", b.progress);
        fm.drawText(*canvas_, x0, y, pg, 1, false);
    }

    // 简介
    y = 360;
    if (!detail_.intro.empty()) {
        fm.drawText(*canvas_, PAD, y, "简介", 1, false);
        y += uiLineHeight();
        y = drawParagraph(detail_.intro.c_str(), PAD, y, SCR_W - 2 * PAD, 5) + 8;
    }

    // 热门评论
    if (!reviews_.empty()) {
        fm.drawText(*canvas_, PAD, y, "热门评论", 1, false);
        y += uiLineHeight();
        for (size_t i = 0; i < reviews_.size() && i < 2 && y < SCR_H - 260; i++) {
            std::string line = reviews_[i].nick + "：" + reviews_[i].content;
            y = drawParagraph(line.c_str(), PAD, y, SCR_W - 2 * PAD, 3) + 6;
        }
    }

    // 底部按钮
    if (detailDeletePending_) {
        // 删除整本下载确认对话框（居中）
        int cx = SCR_W / 2;
        int boxY = SCR_H / 2 - 60;
        canvas_->drawRect(PAD - 2, boxY, SCR_W - 2 * PAD + 4, 120, BLACK);
        fm.drawText(*canvas_, cx - fm.uiTextWidth("确认删除整本下载？") / 2,
                    boxY + 12, "确认删除整本下载？", 1, false);
        // 删除 / 取消 两个按钮
        canvas_->drawRect(cx - 140, boxY + 60, 130, 44, BLACK);
        fm.drawText(*canvas_, cx - 140 + (130 - fm.uiTextWidth("删除")) / 2,
                    boxY + 68, "删除", 1, false);
        canvas_->drawRect(cx + 10, boxY + 60, 130, 44, BLACK);
        fm.drawText(*canvas_, cx + 10 + (130 - fm.uiTextWidth("取消")) / 2,
                    boxY + 68, "取消", 1, false);
    } else {
        canvas_->drawRect(PAD - 2, SCR_H - 130, 220, 66, BLACK);
        canvas_->drawRect(SCR_W - PAD - 220, SCR_H - 130, 220, 66, BLACK);
        const char* readLabel = "继续阅读";
        int rw = fm.uiTextWidth(readLabel);
        fm.drawText(*canvas_, PAD - 2 + (220 - rw) / 2, SCR_H - 116, readLabel, 1, false);
        // 下载按钮：done.flag 与当前章节数一致时显示"已下载"
        bool dlDone = SdCard::instance().ready() &&
                      Storage::instance().bookDownloadDone(b.bookId.c_str(), (int)chapters_.size());
        const char* dlLabel = dlDone ? "已下载" : "下载整本";
        int dw = fm.uiTextWidth(dlLabel);
        fm.drawText(*canvas_, SCR_W - PAD - 220 + (220 - dw) / 2, SCR_H - 116, dlLabel, 1, false);
    }

    // 插图开关
    canvas_->drawRect(PAD - 2, SCR_H - 56, 220, 44, BLACK);
    bool imgoff = false;
    if (SdCard::instance().ready()) imgoff = Storage::instance().loadImgOff(b.bookId.c_str());
    const char* imgt = imgoff ? "插图：关" : "插图：开";
    int iw = fm.uiTextWidth(imgt);
    fm.drawText(*canvas_, PAD - 2 + (220 - iw) / 2, SCR_H - 48, imgt, 1, false);

    // 返回书架
    const char* backLabel = "返回书架";
    int bw = fm.uiTextWidth(backLabel);
    fm.drawText(*canvas_, SCR_W - PAD - bw, SCR_H - 46, backLabel, 1, false);

    canvas_->display(RefreshMode::Full);
}

void WereadApp::openBookDetail(int shelfIdx) {
    bookIdx_ = shelfIdx;
    curBook_ = shelf_[shelfIdx];
    detail_ = weread_api::BookDetail{};
    reviews_.clear();
    prefetch_.clear();  // 切换书时清理预取缓存
    detailDeletePending_ = false;  // 切换书时清除删除确认

    std::string err;

    // 详情（在线时拉取，离线时留空）
    if (!offlineMode_) {
        screenMsg("加载详情...", curBook_.title.c_str());
        for (int i = 0; i < 2; i++) {
            if (weread_api::getBookDetail(curBook_.bookId, detail_, err)) break;
            if (pollTouchDuringDelay(1000)) { renderBookshelf(false); return; }
        }
        for (int i = 0; i < 2; i++) {
            if (weread_api::getHotReviews(curBook_.bookId, reviews_, 2, err)) break;
            if (pollTouchDuringDelay(1000)) { renderBookshelf(false); return; }
        }
    }

    if (!fetchToc(curBook_.bookId)) {
        screenMsg("目录获取失败", err.c_str());
        if (pollTouchDuringDelay(2000)) { renderBookshelf(false); return; }
        renderBookshelf(false);
        return;
    }

    ESP_LOGI(TAG, "详情: %s  %d 章", curBook_.title.c_str(), (int)chapters_.size());
    renderBookDetail();
}

// ================================================================
// 目录
// ================================================================

bool WereadApp::fetchToc(const std::string& bookId) {
    chapters_.clear();
    bookFormat_.clear();
    std::string err;

    // 0. 离线模式：直接读本地目录缓存，跳过在线请求（网络必然失败，避免长时间卡"获取目录"）
    if (offlineMode_ && SdCard::instance().ready() &&
        Storage::instance().loadTocCache(bookId.c_str(), chapters_, bookFormat_)) {
        ESP_LOGI(TAG, "离线目录来自本地缓存: %d 章", (int)chapters_.size());
        return true;
    }

    // 1. 在线获取目录（3 次尝试）
    for (int attempt = 0; attempt < 3 && chapters_.empty(); attempt++) {
        if (attempt && pollTouchDuringDelay(1500 * attempt)) return false;
        weread_api::getChapterInfos(bookId, chapters_, bookFormat_, err);
    }

    if (!chapters_.empty()) {
        // 在线成功 → 写目录缓存（供离线打开已下载的书）
        if (SdCard::instance().ready()) {
            Storage::instance().saveTocCache(bookId.c_str(), chapters_, bookFormat_.c_str());
        }
        return true;
    }

    // 2. 在线失败 → 回退本地目录缓存（离线阅读已下载的书，无需网络）
    if (SdCard::instance().ready() &&
        Storage::instance().loadTocCache(bookId.c_str(), chapters_, bookFormat_)) {
        ESP_LOGI(TAG, "目录来自本地缓存: %d 章", (int)chapters_.size());
        return true;
    }

    ESP_LOGW(TAG, "目录获取失败: %s (bookId=%s)", err.c_str(), bookId.c_str());
    return false;
}

void WereadApp::renderToc() {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::Toc;
    canvas_->clear();

    auto& fm = FontManager::instance();
    fm.drawText(*canvas_, PAD, 16, "目录", 1, false);
    std::string title = curBook_.title;
    truncatePx(title, 240);
    fm.drawText(*canvas_, PAD + 90, 16, title.c_str(), 1, false);

    // 右上角：返回 + 状态栏
    drawStatusBar(14);
    const char* backLabel = "返回";
    int bw = fm.uiTextWidth(backLabel);
    fm.drawText(*canvas_, SCR_W - PAD - bw, 16, backLabel, 1, false);

    int per = (SCR_H - TOC_LIST_Y0 - 70) / (uiLineHeight() + 10);
    int totalPages = std::max(1, ((int)chapters_.size() + per - 1) / per);
    if (tocPage_ < 0) tocPage_ = 0;
    if (tocPage_ >= totalPages) tocPage_ = totalPages - 1;

    int start = tocPage_ * per;
    int end = std::min(start + per, (int)chapters_.size());
    int y = TOC_LIST_Y0;
    int lh = uiLineHeight() + 10;
    for (int i = start; i < end; i++) {
        std::string line = (i == chapterIdx_ ? "→ " : "  ") + chapters_[i].title;
        truncatePx(line, 420);
        fm.drawText(*canvas_, PAD, y, line.c_str(), 1, false);
        y += lh;
    }

    // 底部翻页
    fm.drawText(*canvas_, PAD, SCR_H - 40, "上一页", 1, false);
    char pg[32];
    snprintf(pg, sizeof(pg), "%d/%d", tocPage_ + 1, totalPages);
    int pgw = fm.uiTextWidth(pg);
    fm.drawText(*canvas_, (SCR_W - pgw) / 2, SCR_H - 40, pg, 1, false);
    const char* nextLabel = "下一页";
    int nw = fm.uiTextWidth(nextLabel);
    fm.drawText(*canvas_, SCR_W - PAD - nw, SCR_H - 40, nextLabel, 1, false);

    canvas_->display(RefreshMode::Fast);
}

// ================================================================
// 阅读页 — 分页 + 渲染
// ================================================================

static bool isParaStart(const std::string& text, size_t idx) {
    return idx == 0 || text[idx - 1] == '\n';
}

void WereadApp::paginateCurrent() {
    pages_.clear();
    pages_.push_back({0, 0});
    Cursor cur{0, 0};
    int y = 0, H = pageAreaHeight(), maxW = pageMaxWidth();
    int lh = readLineHeight(), pgGap = paraGap();
    bool imgOff = false;
    if (SdCard::instance().ready()) imgOff = Storage::instance().loadImgOff(curBook_.bookId.c_str());

    while (cur.blk < blocks_.size()) {
        auto& b = blocks_[cur.blk];
        if (b.is_image) {
            if (imgOff) { cur.blk++; cur.off = 0; continue; }
            int need = 300 + lh / 2;  // 估算图片高度
            if (y + need > H && y > 0) {
                pages_.push_back({cur.blk, 0});
                y = 0;
                continue;
            }
            y += need;
            cur.blk++; cur.off = 0;
        } else {
            if (cur.off >= b.text.size()) { cur.blk++; cur.off = 0; continue; }
            uint8_t st = b.style;
            int blh = (st == 1) ? lh * 2 : lh;
            float wsc = (st == 1) ? 2.0f : 1.0f;
            bool para = (st == 0) && isParaStart(b.text, cur.off);
            int indent = para ? paraIndentWidth() : 0;
            size_t end = nextLineEnd(b.text, cur.off, maxW - indent, wsc);
            std::string line = b.text.substr(cur.off, end - cur.off);
            // trim
            size_t s = line.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) {
                size_t f = line.find_last_not_of(" \t\r\n");
                line = line.substr(s, f - s + 1);
            } else {
                line = "";
            }
            if (!line.empty()) {
                int need = blh + ((para && y > 0) ? pgGap : 0);
                if (y + need > H && y > 0) {
                    pages_.push_back({cur.blk, cur.off});
                    y = 0;
                    continue;
                }
                y += need;
            }
            cur.off = end;
        }
    }
}

void WereadApp::renderReadingPage(RefreshMode mode, bool wait) {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::Reading;
    canvas_->clear();

    auto& fm = FontManager::instance();

    // 标题
    std::string title = chapterTitle_;
    truncatePx(title, 240);
    fm.drawText(*canvas_, PAD, 14, title.c_str(), 1, true);
    drawStatusBar(12);
    canvas_->drawLine(PAD - 12, 56, SCR_W - PAD, 56, BLACK);

    int lh = readLineHeight(), pgGap = paraGap();
    int maxW = pageMaxWidth();
    int yLimit = 80 + pageAreaHeight();
    int y = 80;
    bool imgOff = false;
    if (SdCard::instance().ready()) imgOff = Storage::instance().loadImgOff(curBook_.bookId.c_str());

    if (page_ < 0 || page_ >= (int)pages_.size()) page_ = 0;
    Cursor cur = pages_[page_];

    while (cur.blk < blocks_.size()) {
        auto& b = blocks_[cur.blk];
        if (b.is_image) {
            if (imgOff) { cur.blk++; cur.off = 0; continue; }
            // 尝试渲染图片
            bool drew = false;
            if (!b.text.empty() && SdCard::instance().ready()) {
                char thumbPath[256];
                if (ImageRenderer::instance().getThumbnail(b.text.c_str(), thumbPath, sizeof(thumbPath))) {
                    drew = ImageRenderer::instance().renderFromFile(*canvas_, thumbPath, BODY_MARGIN, y, maxW, pageAreaHeight() - 120);
                }
            }
            int h = drew ? 300 : lh;  // 简化：用估算高度
            if (!drew) {
                fm.drawText(*canvas_, BODY_MARGIN, y, "[图片]", 1, false);
                h = lh;
            }
            y += h + lh / 2;
            cur.blk++; cur.off = 0;
            if (y > SCR_H - 60) break;
        } else {
            if (cur.off >= b.text.size()) { cur.blk++; cur.off = 0; continue; }
            uint8_t st = b.style;
            int blh = (st == 1) ? lh * 2 : lh;
            float wsc = (st == 1) ? 2.0f : 1.0f;
            bool para = (st == 0) && isParaStart(b.text, cur.off);
            int indent = para ? paraIndentWidth() : 0;
            size_t end = nextLineEnd(b.text, cur.off, maxW - indent, wsc);
            std::string line = b.text.substr(cur.off, end - cur.off);
            size_t s = line.find_first_not_of(" \t\r\n");
            if (s != std::string::npos) {
                size_t f = line.find_last_not_of(" \t\r\n");
                line = line.substr(s, f - s + 1);
            } else {
                line = "";
            }
            if (!line.empty()) {
                int need = blh + ((para && y > 80) ? pgGap : 0);
                if (y + need > yLimit && y > 80) break;
                if (para && y > 80) y += pgGap;
                // 按样式渲染
                if (st == 1) {
                    fm.drawTextBody(*canvas_, BODY_MARGIN + indent, y, line.c_str(), 2, true);
                } else if (st >= 2 && st <= 4) {
                    fm.drawTextBody(*canvas_, BODY_MARGIN + indent, y, line.c_str(), 1, true);
                } else {
                    fm.drawTextBody(*canvas_, BODY_MARGIN + indent, y, line.c_str(), 1, false);
                }
                y += blh;
            }
            cur.off = end;
        }
    }

    // 页脚
    fm.drawText(*canvas_, PAD, SCR_H - 40, "版式", 1, false);
    char pg[32];
    snprintf(pg, sizeof(pg), "%d/%d", page_ + 1, (int)pages_.size());
    int pgw = fm.uiTextWidth(pg);
    fm.drawText(*canvas_, (SCR_W - pgw) / 2, SCR_H - 40, pg, 1, false);
    const char* shelfLabel = "书架";
    int sw = fm.uiTextWidth(shelfLabel);
    fm.drawText(*canvas_, SCR_W - PAD - sw, SCR_H - 40, shelfLabel, 1, false);

    if (wait) {
        canvas_->display(mode);
    } else {
        canvas_->displayAsync(mode);
    }
}

void WereadApp::turnPage(int delta) {
    if (currentScreen_ != ScreenId::Reading || pages_.empty()) return;
    int np = page_ + delta;
    if (np >= (int)pages_.size()) {
        if (chapterIdx_ + 1 < (int)chapters_.size()) {
            triggerPrefetch();  // 确保下一章预取已启动
            openChapter(chapterIdx_ + 1);
            return;
        }
        if (page_ == (int)pages_.size() - 1) return;
        np = pages_.size() - 1;
    } else if (np < 0) {
        if (chapterIdx_ > 0) {
            openChapter(chapterIdx_ - 1);
            return;
        }
        if (page_ == 0) return;
        np = 0;
    }
    page_ = np;
    fastCount_++;
    RefreshMode mode = (fastCount_ % FULL_REFRESH_EVERY == 0) ? RefreshMode::Full : RefreshMode::Fast;
    // 同步刷新：等待本帧刷新完成才返回，确保每次翻页都真正刷新，
    // 避免 EPD 忙时推送新帧导致刷新命令被吞（画面残留、后页不刷新）
    // 刷新期间挂起预取任务，防止后台任务占用 CPU/SPI 干扰刷新时序
    if (prefetchTask_) vTaskSuspend(prefetchTask_);
    renderReadingPage(mode, true);
    if (prefetchTask_) vTaskResume(prefetchTask_);
    saveProgress();
    triggerPrefetch();  // 翻页接近章末时后台预取下一章
}

// ================================================================
// 后台预取下一章（Task 10）
// ================================================================

void WereadApp::prefetchTaskFunc() {
    ESP_LOGI(TAG, "预取任务启动");
    while (true) {
        if (xSemaphoreTake(prefetchSemaphore_, portMAX_DELAY) != pdTRUE) continue;

        int nextIdx = chapterIdx_ + 1;
        if (nextIdx >= (int)chapters_.size()) continue;
        if (prefetch_.valid && prefetch_.chapterIdx == nextIdx) continue;
        if (prefetch_.loading) continue;

        prefetch_.loading = true;
        prefetch_.chapterIdx = nextIdx;
        prefetch_.blocks.clear();
        prefetch_.title = chapters_[nextIdx].title;

        ESP_LOGI(TAG, "后台预取第 %d 章: %s", nextIdx, prefetch_.title.c_str());

        std::string err;
        std::vector<weread_api::ContentBlock> tmpBlocks;
        if (weread_api::getChapterBlocks(curBook_.bookId, chapters_[nextIdx], tmpBlocks, err)) {
            prefetch_.blocks = std::move(tmpBlocks);
            prefetch_.valid = true;
            ESP_LOGI(TAG, "预取完成: %d 块", (int)prefetch_.blocks.size());
            // 预取成功也写入 SD 缓存（供离线阅读）
            if (SdCard::instance().ready() && !curBook_.bookId.empty()) {
                Storage::instance().chapterCacheSave(curBook_.bookId.c_str(),
                                                     chapters_[nextIdx].chapterUid.c_str(),
                                                     prefetch_.blocks);
            }
        } else {
            ESP_LOGW(TAG, "预取失败: %s", err.c_str());
            prefetch_.valid = false;
            prefetch_.failedErr = err;  // 存储错误，供 openChapter 跳过冗余重试
        }
        prefetch_.loading = false;
    }
}

// 后台自动重连：设备离线/掉线时，周期（30s）尝试联网，不阻塞 UI。
// 已联网则跳过；connectMulti 阻塞在本任务内，不影响主线程渲染。
void WereadApp::wifiReconnectTaskFunc() {
    ESP_LOGI(TAG, "WiFi 自动重连任务启动");
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(30000));
        WifiManager& wm = WifiManager::instance();
        if (wm.isConnected()) continue;
        if (!wm.backgroundReconnectDue()) continue;
        wm.markReconnectAttempt();
        ESP_LOGI(TAG, "后台自动重连...");
        WifiState st = wm.connectMulti();
        if (st == WifiState::Connected) {
            ESP_LOGI(TAG, "后台自动重连成功");
            wifiConnected_ = true;
            offlineMode_ = false;
        }
    }
}

void WereadApp::triggerPrefetch() {
    if (!prefetchSemaphore_) return;
    if (chapters_.empty() || chapterIdx_ + 1 >= (int)chapters_.size()) return;

    // 剩余页数 ≤ 2 时触发
    int remaining = (int)pages_.size() - page_ - 1;
    if (remaining > 2) return;

    // 已经预取过或正在预取，不重复
    if (prefetch_.valid && prefetch_.chapterIdx == chapterIdx_ + 1) return;
    if (prefetch_.loading) return;

    ESP_LOGI(TAG, "触发预取 (剩余 %d 页)", remaining);
    xSemaphoreGive(prefetchSemaphore_);
}

bool WereadApp::tryConsumePrefetch(int chIdx) {
    // 预取任务可能正在写入，避免竞争
    if (prefetch_.loading) return false;
    if (!prefetch_.valid || prefetch_.chapterIdx != chIdx) return false;
    if (prefetch_.blocks.empty()) return false;

    ESP_LOGI(TAG, "预取缓存命中! ch=%d", chIdx);
    blocks_ = std::move(prefetch_.blocks);
    chapterTitle_ = prefetch_.title;
    prefetch_.clear();
    return true;
}

void WereadApp::openChapter(int chIdx, int page) {
    if (chapters_.empty()) return;
    if (chIdx < 0) chIdx = 0;
    if (chIdx >= (int)chapters_.size()) chIdx = chapters_.size() - 1;

    chapterIdx_ = chIdx;
    blocks_.clear();

    // 1. 先尝试预取缓存命中（命中则跳过在线请求，实现秒开）
    if (tryConsumePrefetch(chIdx)) {
        page_ = 0;
        paginateCurrent();
        if (page > 0 && page < (int)pages_.size()) page_ = page;
        renderReadingPage(RefreshMode::Full, true);
        saveProgress();
        triggerPrefetch();  // 继续预取下下一章
        return;
    }

    // 1.2 预取未命中时，尝试 SD 章节缓存（离线阅读）
    std::vector<weread_api::ContentBlock> cached;
    if (SdCard::instance().ready() &&
        Storage::instance().chapterCacheLoad(curBook_.bookId.c_str(),
                                             chapters_[chIdx].chapterUid.c_str(), cached)) {
        blocks_ = std::move(cached);
        chapterTitle_ = chapters_[chIdx].title;
        page_ = 0;
        paginateCurrent();
        if (page > 0 && page < (int)pages_.size()) page_ = page;
        renderReadingPage(RefreshMode::Full, true);
        saveProgress();
        triggerPrefetch();  // 下一章仍尝试后台预取（若已缓存则不需要网络）
        return;
    }

    // 1.5 预取已失败（如会员章节 e_0），直接显示错误，点击返回主页
    if (!prefetch_.loading && prefetch_.chapterIdx == chIdx && !prefetch_.failedErr.empty()) {
        const std::string& ferr = prefetch_.failedErr;
        bool isMember = (ferr.find("e_0") != std::string::npos);
        screenMsg("正文获取失败", isMember ? "本章为会员章节" : ferr.c_str(), "点按返回主页");
        waitTouchBackHome();
        return;
    }

    // 2. 未命中，走正常在线请求
    chapterTitle_ = chapters_[chIdx].title;
    std::string err;
    screenMsg("拉取正文...", chapterTitle_.c_str());

    if (!weread_api::getChapterBlocks(curBook_.bookId, chapters_[chIdx], blocks_, err)) {
        // 重试期间允许点击返回主页
        if (pollTouchDuringDelay(2000)) {
            renderHome();
            return;
        }
        if (!weread_api::getChapterBlocks(curBook_.bookId, chapters_[chIdx], blocks_, err)) {
            bool isMember = (err.find("e_0") != std::string::npos);
            screenMsg("正文获取失败", isMember ? "本章为会员章节" : err.c_str(), "点按返回主页");
            waitTouchBackHome();
            return;
        }
    }

    if (blocks_.empty()) {
        screenMsg("本章无内容", chapterTitle_.c_str());
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    // 在线拉取成功 → 写入 SD 缓存（供离线阅读）
    if (SdCard::instance().ready() && !curBook_.bookId.empty()) {
        Storage::instance().chapterCacheSave(curBook_.bookId.c_str(),
                                             chapters_[chIdx].chapterUid.c_str(), blocks_);
    }

    page_ = 0;
    paginateCurrent();
    if (page > 0 && page < (int)pages_.size()) page_ = page;

    renderReadingPage(RefreshMode::Full, true);
    saveProgress();

    // 3. 渲染后触发下一章预取
    triggerPrefetch();
}

void WereadApp::continueReading() {
    if (curBook_.bookId.empty() || chapters_.empty()) return;

    int chIdx = 0, page = 0;
    // 本地进度优先：在本设备读过则直接用本地章节/页码
    bool hasLoc = false;
    BookProgress bp;
    if (SdCard::instance().ready()) {
        hasLoc = Storage::instance().loadProgress(curBook_.bookId.c_str(), bp);
        if (hasLoc) { chIdx = bp.chapterIdx; page = bp.page; }
    }

    if (!hasLoc) {
        // 无本地记录：用服务器进度章节定位（可能是其它设备读的）
        int srvCh = -1;
        if (!curBook_.progressChapterUid.empty()) {
            for (int i = 0; i < (int)chapters_.size(); i++) {
                if (chapters_[i].chapterUid == curBook_.progressChapterUid) {
                    srvCh = i;
                    break;
                }
            }
        }
        if (srvCh >= 0) chIdx = srvCh;
    }

    if (chIdx >= (int)chapters_.size()) chIdx = 0;
    ESP_LOGI(TAG, "继续阅读 ch=%d page=%d", chIdx, page);
    openChapter(chIdx, page);
}

void WereadApp::saveProgress() {
    if (curBook_.bookId.empty() || !SdCard::instance().ready()) return;
    BookProgress bp = {};
    bp.chapterIdx = chapterIdx_;
    bp.page = page_;
    strncpy(bp.title, curBook_.title.c_str(), sizeof(bp.title) - 1);
    // 计算真实阅读进度百分比：基于本地已加载目录与当前章总页数
    // 进度 = (已读章节比例 + 当前章内页比例) / 总章节数
    if (chapters_.size() > 0 && pages_.size() > 0) {
        int64_t ch = (int64_t)chapterIdx_ * 100;
        int64_t pg = (int64_t)(page_ + 1) * 100 / pages_.size();
        bp.percent = (int)((ch + pg) / chapters_.size());
        if (bp.percent < 0) bp.percent = 0;
        if (bp.percent > 100) bp.percent = 100;
    } else {
        bp.percent = -1;  // 未知，主页用服务器进度兜底
    }
    Storage::instance().saveProgress(curBook_.bookId.c_str(), bp);

    // 写恢复标记
    ReadingState st = {};
    strncpy(st.bookId, curBook_.bookId.c_str(), sizeof(st.bookId) - 1);
    st.chapterIdx = chapterIdx_;
    st.page = page_;
    strncpy(st.title, curBook_.title.c_str(), sizeof(st.title) - 1);
    strncpy(st.cover, curBook_.cover.c_str(), sizeof(st.cover) - 1);
    strncpy(st.format, bookFormat_.c_str(), sizeof(st.format) - 1);
    Storage::instance().saveState(st);
}

bool WereadApp::tryRestoreReading() {
    if (!SdCard::instance().ready()) return false;
    ReadingState st;
    if (!Storage::instance().loadState(st)) return false;
    if (strlen(st.bookId) == 0) return false;

    ESP_LOGI(TAG, "恢复阅读: %s ch=%d p=%d", st.bookId, st.chapterIdx, st.page);

    // 重建 curBook_
    curBook_.bookId = st.bookId;
    curBook_.title = st.title;
    curBook_.cover = st.cover;
    curBook_.format = st.format;
    bookFormat_ = st.format;

    // 离线也可恢复：fetchToc 离线时直接读本地目录缓存，openChapter 读章节缓存
    if (!fetchToc(curBook_.bookId)) {
        ESP_LOGW(TAG, "恢复阅读失败：目录获取失败（离线且无目录缓存）");
        return false;
    }

    openChapter(st.chapterIdx, st.page);
    Storage::instance().removeState();  // 恢复后清除标记
    return true;
}

// ================================================================
// 字体/版式页
// ================================================================

void WereadApp::drawCapsule(int x, int y, int w, bool selected) {
    const int H = 32;
    if (selected) {
        canvas_->fillRect(x, y, w, H, BLACK);
    } else {
        canvas_->drawRect(x, y, w, H, BLACK);
        canvas_->drawRect(x + 1, y + 1, w - 2, H - 2, BLACK);
    }
}

void WereadApp::loadFontList() {
    fontList_.clear();
    if (!SdCard::instance().ready()) return;

    // 目录项数组 ~17KB，放堆上避免爆主任务栈
    SdCard::DirEntry* entries =
        (SdCard::DirEntry*)calloc(64, sizeof(SdCard::DirEntry));
    if (!entries) return;
    int n = SdCard::instance().listDir("/", entries, 64);
    for (int i = 0; i < n; i++) {
        if (!entries[i].isDir) {
            std::string name = entries[i].name;
            // 过滤 macOS 元数据 + 只保留 .bin/.eefont
            if (name.rfind("._", 0) != 0 &&
                (name.find(".bin") != std::string::npos ||
                 name.find(".eefont") != std::string::npos)) {
                fontList_.push_back("/" + name);
            }
        }
    }
    free(entries);
    std::sort(fontList_.begin(), fontList_.end());
    ESP_LOGI(TAG, "发现 %d 个字体", (int)fontList_.size());
}

void WereadApp::renderFontSelect() {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::FontSelect;
    canvas_->clear();

    auto& fm = FontManager::instance();
    fm.drawText(*canvas_, PAD, 16, "版式", 1, false);
    // 右上角 × 关闭
    fm.drawText(*canvas_, SCR_W - PAD - 20, 12, "x", 1, false);
    drawStatusBar(14);

    // 版式三行
    static const char* LINE_NAMES[] = {"紧凑", "标准", "宽松"};
    static const char* GAP_NAMES[] = {"无", "四分之一", "半行"};
    static const char* INDENT_NAMES[] = {"无", "1 字符", "2 字符"};

    int y = 96;
    auto layoutRow = [&](const char* label, int val, const char* names[3]) {
        fm.drawText(*canvas_, PAD, y + 4, label, 1, false);
        int cx = SCR_W - PAD - 3 * 100 - 2 * 8;
        for (int i = 0; i < 3; i++) {
            drawCapsule(cx, y, 100, i == val);
            // 胶囊内文字
            int tw = fm.uiTextWidth(names[i]);
            uint8_t color = (i == val) ? WHITE : BLACK;
            fm.drawText(*canvas_, cx + (100 - tw) / 2, y + 6, names[i], 1, i == val);
            cx += 100 + 8;
        }
        y += LAYOUT_ROW_H;
    };
    layoutRow("行间距", lineSpacing_, LINE_NAMES);
    layoutRow("段间距", paraGapMode_, GAP_NAMES);
    layoutRow("首行缩进", paraIndent_, INDENT_NAMES);

    canvas_->drawLine(PAD, y + 4, SCR_W - PAD, y + 4, BLACK);
    y += 16;

    // 字体列表
    for (size_t i = 0; i < fontList_.size() && y < SCR_H - 60; i++) {
        std::string name = fontList_[i];
        size_t slash = name.find_last_of('/');
        if (slash != std::string::npos) name = name.substr(slash + 1);

        bool cur = (fontList_[i] == bodyFontPath_);
        if (cur) fm.drawText(*canvas_, PAD, y, "v", 1, false);

        // 用正文字体（已加载）渲染当前字体名；非当前字体用 UI 字体
        if (cur) {
            fm.drawTextBody(*canvas_, PAD + 36, y, name.c_str(), 1, false);
        } else {
            fm.drawText(*canvas_, PAD + 36, y, name.c_str(), 1, false);
        }
        y += FONT_ROW_H;
    }

    canvas_->display(RefreshMode::Full);
}

void WereadApp::applyFont(int idx) {
    if (idx < 0 || idx >= (int)fontList_.size()) return;
    ESP_LOGI(TAG, "切换字体 → %s", fontList_[idx].c_str());

    if (FontManager::instance().loadBodyFont(fontList_[idx].c_str())) {
        bodyFontPath_ = fontList_[idx];
        strncpy(Config::instance().mutableGet().fontPath, fontList_[idx].c_str(),
                sizeof(AppConfig::fontPath) - 1);
        Config::instance().save();
    } else {
        ESP_LOGW(TAG, "字体加载失败，回滚");
        FontManager::instance().loadBodyFont(bodyFontPath_.c_str());
        screenMsg("字体加载失败", fontList_[idx].c_str());
        vTaskDelay(pdMS_TO_TICKS(1500));
    }

    paginateCurrent();
    if (page_ >= (int)pages_.size()) page_ = pages_.size() - 1;
    if (page_ < 0) page_ = 0;
    backToReading();
}

void WereadApp::applyLayout() {
    AppConfig& cfg = Config::instance().mutableGet();
    cfg.lineHeight = lineSpacing_;
    cfg.paragraphSpacing = paraGapMode_;
    cfg.firstLineIndent = paraIndent_;
    Config::instance().save();

    paginateCurrent();
    if (page_ >= (int)pages_.size()) page_ = pages_.size() - 1;
    if (page_ < 0) page_ = 0;
}

void WereadApp::backToReading() {
    currentScreen_ = ScreenId::Reading;
    renderReadingPage(RefreshMode::Full, true);
}

// ================================================================
// 设置页 Settings
// ================================================================

void WereadApp::renderSettings() {
    previousScreen_ = currentScreen_;
    currentScreen_ = ScreenId::Settings;
    canvas_->clear();

    auto& fm = FontManager::instance();

    // 1. 返回箭头（左上角）
    canvas_->drawIcon(36, 32, ICON_CHEVRON_LEFT_32, 32, 32, BLACK);

    // 2. 标题 x=88 y=32 "设置" scale=1 加粗
    fm.drawText(*canvas_, 88, 36, "设置", 1, true);

    // 3. 6个条目（每个 w=496 h=64 圆角16 灰底，无边框）
    struct SettingItem {
        int16_t y;
        const char* mainText;
        const char* subText;
        bool bold;
    };
    SettingItem items[] = {
        {100, "退出登录", nullptr,             false},
        {180, "WiFi 重连", "用已保存配置重连",  false},
        {260, "WiFi 重置", "清除配置重新配网",  false},
        {340, "重置设备", "清空缓存与账号",     true},
        {420, "检查更新", FW_VERSION,           false},
        {500, "返回主页", nullptr,             false},
    };

    for (int i = 0; i < 6; i++) {
        auto& item = items[i];
        // 灰底圆角16（无边框）
        fillGrayRound(canvas_, 28, item.y, 496, 64, 16);

        // 主文案 x=52 y+20 scale=1
        fm.drawText(*canvas_, 52, item.y + 22, item.mainText, 1, item.bold);

        // 副文案：主文案右侧、同大小深灰色（原为下行小字，改为右侧深灰）
        if (item.subText) {
            int mw = fm.uiTextWidth(item.mainText);
            fm.drawTextGray(*canvas_, 52 + mw + 20, item.y + 22, item.subText, 1, 6);
        }

        // 右箭头 x=488 y+24
        canvas_->drawIcon(488, item.y + 24, ICON_CHEVRON_RIGHT_16, 16, 16, BLACK);
    }

    // 4. 底部提示 x=126 y=730 居中
    const char* hint = settingsResetPending_
        ? "再次点击「重置设备」确认（5秒内）"
        : "恢复出厂设置需长按确认";
    canvas_->drawTextCentered(126, 730, 300, hint, 1, false);

    // 5. 版本号 x=126 y=750 居中（底部提示下方）
    canvas_->drawTextCentered(126, 750, 300, "版本: " FW_VERSION, 1, false);

    canvas_->display(RefreshMode::Full);
}

// ================================================================
// 扫码登录
// ================================================================

// 嵌入式 6x8 英文点阵渲染（无 SD 卡/中文字体不可用时的兜底）
void WereadApp::drawAsciiFallback(const char* text, int16_t x, int16_t y,
                                  int scale, bool bold) {
    if (!text || scale < 1) return;
    for (const char* p = text; *p; p++) {
        uint8_t c = (uint8_t)*p;
        if (c < ASCII_FONT_FIRST || c >= ASCII_FONT_FIRST + ASCII_FONT_COUNT) {
            c = ' ';
        }
        const uint8_t* col = ASCII_FONT[c - ASCII_FONT_FIRST];
        for (int cx = 0; cx < ASCII_FONT_W; cx++) {
            for (int cy = 0; cy < ASCII_FONT_H; cy++) {
                if (!(col[cx] & (1 << cy))) continue;
                canvas_->fillRect(x + cx * scale, y + cy * scale,
                                  scale, scale, BLACK);
                if (bold) {
                    canvas_->fillRect(x + cx * scale + scale, y + cy * scale,
                                      scale, scale, BLACK);
                }
            }
        }
        x += (ASCII_FONT_W + 1) * scale;
    }
}

// SD 卡未挂载：无法加载中文字体/配置，用内置英文字库提示，点按屏幕重启
void WereadApp::showNoSdScreen() {
    canvas_->clear();
    auto centered = [&](const char* s, int scale) {
        return (SCR_W - (int)strlen(s) * (ASCII_FONT_W + 1) * scale) / 2;
    };
    drawAsciiFallback("NO SD CARD", centered("NO SD CARD", 5), 210, 5, true);
    drawAsciiFallback("PLEASE INSERT THE SD CARD",
                      centered("PLEASE INSERT THE SD CARD", 2), 330, 2, false);
    drawAsciiFallback("TAP THE SCREEN TO RESTART",
                      centered("TAP THE SCREEN TO RESTART", 2), 366, 2, false);
    canvas_->display(RefreshMode::Full);
    currentScreen_ = ScreenId::Provision;

    ESP_LOGI(TAG, "SD 卡未挂载，等待点按重启");
    for (;;) {
        TouchEvent evt = touch_->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            ESP_LOGI(TAG, "点按重启");
            esp_restart();
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

// 配网引导屏：设备屏幕显示 WIFI 格式二维码，
// 手机扫码后自动连接热点 "EEGO-A4-阅读器"，
// 手机系统 captive portal 检测会自动弹出配网页（web 验证方式）。
// 手动连接热点后打开任意网页（DNS 劫持）也会 302 跳转到配网页。
// 说明文字用正文字体（24px）而非 UI 字体（31px），避免整屏过大。
void WereadApp::renderProvisionGuide() {
    canvas_->clear();
    auto& fm = FontManager::instance();

    // 1. 标题 x=76 y=72 w=400 scale=1 加粗 居中
    canvas_->drawTextCentered(76, 76, 400, "连接 WiFi 配置网络", 1, true);

    // 2. 二维码外框 x=141 y=136 w=270 h=270 白底（无边框）
    const char* qrText = "WIFI:T:nopass;S:EEGO-A4-阅读器;;";
    try {
        qrcodegen::QrCode qr =
            qrcodegen::QrCode::encodeText(qrText, qrcodegen::QrCode::Ecc::LOW);
        int size = qr.getSize();
        int maxPx = 250;
        int scale = maxPx / size;
        if (scale < 2) scale = 2;
        int px = scale * size;
        int ox = 141 + (270 - px) / 2;
        int oy = 136 + (270 - px) / 2;

        // 白底（无边框）
        canvas_->fillRect(141, 136, 270, 270, WHITE);

        for (int yy = 0; yy < size; yy++) {
            for (int xx = 0; xx < size; xx++) {
                if (qr.getModule(xx, yy)) {
                    canvas_->fillRect(ox + xx * scale, oy + yy * scale,
                                      scale, scale, BLACK);
                }
            }
        }
    } catch (...) {
        ESP_LOGW(TAG, "配网二维码生成失败");
        fm.drawText(*canvas_, 141, 250, "二维码生成失败", 1, false);
    }

    // 3. 说明文字 x=76 y=444 w=400 两行 居中
    canvas_->drawTextCentered(76, 444, 400, "手机连接热点 EEGO-A4", 1, false);
    canvas_->drawTextCentered(76, 488, 400, "访问 192.168.4.1 配置 WiFi", 1, false);

    // 4. 欢迎语 x=196 y=700 w=160 scale=1 加粗 居中
    canvas_->drawTextCentered(196, 704, 160, "欢迎", 1, true);

    canvas_->display(RefreshMode::Full);
    currentScreen_ = ScreenId::Provision;
}

bool WereadApp::doQrLogin() {
    // QR 渲染回调（设计稿布局）
    auto renderQr = [this](const std::string& url, const std::string& status) {
        canvas_->clear();
        auto& fm = FontManager::instance();

        // 1. 标题 x=96 y=72 w=360 scale=1 加粗 居中
        canvas_->drawTextCentered(96, 76, 360, "微信扫码登录", 1, true);

        try {
            qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(url.c_str(), qrcodegen::QrCode::Ecc::LOW);
            int size = qr.getSize();
            // QR 外框 x=141 y=140 w=270 h=270，QR 在框内居中
            int maxPx = 250;  // 留 10px 内边距
            int scale = maxPx / size;
            if (scale < 2) scale = 2;
            int px = scale * size;
            int ox = 141 + (270 - px) / 2;
            int oy = 140 + (270 - px) / 2;

            // 白底（无边框）
            canvas_->fillRect(141, 140, 270, 270, WHITE);

            // QR 内容居中渲染
            for (int yy = 0; yy < size; yy++) {
                for (int xx = 0; xx < size; xx++) {
                    if (qr.getModule(xx, yy)) {
                        canvas_->fillRect(ox + xx * scale, oy + yy * scale, scale, scale, BLACK);
                    }
                }
            }
        } catch (...) {
            fm.drawText(*canvas_, 141, 250, "二维码生成失败", 1, false);
        }

        // 3. 状态文字 x=96 y=456 w=360 居中
        canvas_->drawTextCentered(96, 456, 360, status.c_str(), 1, false);

        // 4. 跳过登录按钮 x=196 y=640 w=160 h=48 圆角24 灰底（无边框）
        fillGrayRound(canvas_, 196, 640, 160, 48, 24);
        canvas_->drawTextCentered(196, 656, 160, "跳过登录", 1, false);

        // 5. 底部提示 x=76 y=708 w=400 居中
        canvas_->drawTextCentered(76, 708, 400, "登录后自动同步书架与阅读进度", 1, false);

        canvas_->display(RefreshMode::Full);
    };

    auto msg = [this](const std::string& l1, const std::string& l2, const std::string& l3) {
        screenMsg(l1.c_str(), l2.c_str(), l3.c_str());
    };

    for (;;) {
        if (weread_login::run(renderQr, msg)) return true;
        ESP_LOGW(TAG, "登录未完成，等待点按重试");
        // 等待点按（120s 超时）
        int64_t t0 = esp_timer_get_time();
        for (;;) {
            TouchEvent evt = touch_->poll();
            if (evt.type == TouchEventType::Touch && evt.pressed) break;
            if ((esp_timer_get_time() - t0) / 1000 > 120000) return false;
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

// ================================================================
// 睡眠
// ================================================================

void WereadApp::enterSleep() {
    // 保存当前阅读进度到 state.json
    saveProgress();
    // 调用 SleepManager 进入浅睡
    SleepManager::instance().enterLightSleep();
}

// 浅睡前：显示当前书封面 + EPD 断电
void WereadApp::displaySleepCover() {
    ESP_LOGI(TAG, "显示休眠封面");
    canvas_->clear();
    auto& fm = FontManager::instance();

    if (!curBook_.bookId.empty()) {
        // 封面
        bool drew = false;
        if (!curBook_.cover.empty() && SdCard::instance().ready()) {
            char thumbPath[256];
            if (ImageRenderer::instance().getThumbnail(curBook_.cover.c_str(), thumbPath, sizeof(thumbPath))) {
                drew = ImageRenderer::instance().renderFromFile(*canvas_, thumbPath, 0, 60, SCR_W, SCR_H - 220);
            }
        }
        if (!drew) {
            std::string t = curBook_.title;
            truncatePx(t, 400);
            int tw = fm.uiTextWidth(t.c_str());
            fm.drawText(*canvas_, (SCR_W - tw) / 2, 430, t.c_str(), 1, false);
        }
        std::string t = curBook_.title;
        truncatePx(t, 460);
        fm.drawText(*canvas_, PAD, SCR_H - 120, t.c_str(), 1, false);
        fm.drawText(*canvas_, PAD, SCR_H - 64, "已休眠 · 按电源键唤醒", 1, false);
    } else {
        const char* t = "微信读书";
        int tw = fm.uiTextWidth(t);
        fm.drawText(*canvas_, (SCR_W - tw) / 2, 430, t, 1, false);
        const char* s = "已休眠";
        int sw = fm.uiTextWidth(s);
        fm.drawText(*canvas_, (SCR_W - sw) / 2, 510, s, 1, false);
    }

    canvas_->display(RefreshMode::Full);
    canvas_->waitRefreshComplete();

    // EPD 断电（墨水屏图像永久保持）
    display_->deepSleep();
}

// 唤醒后：恢复 EPD 电源 + 重绘当前页
void WereadApp::onWakeFromSleep() {
    ESP_LOGI(TAG, "唤醒：恢复 UI 显示");
    // EPD 重新上电（display_->begin 会恢复电源）
    // 注意：Uc8279cDisplay::deepSleep() 后需要重新初始化控制器
    display_->begin();
    canvas_->resetClip();

    // 先恢复 UI 显示（阅读页/主页/书架页），WiFi 重连挪到后台静默进行
    if (currentScreen_ == ScreenId::Reading && !pages_.empty()) {
        renderReadingPage(RefreshMode::Full, true);
    } else if (currentScreen_ == ScreenId::Home) {
        renderHome();
    } else {
        renderBookshelf(false);
    }
}

// ================================================================
// 触控处理
// ================================================================

void WereadApp::handleTouch(const TouchEvent& evt) {
    if (evt.type == TouchEventType::Touch && !evt.pressed) return;
    if (evt.type == TouchEventType::FrontKeyLong) {
        enterSleep();
        return;
    }
    if (evt.type == TouchEventType::FrontKeyShort) {
        if (currentScreen_ == ScreenId::Reading) turnPage(1);
        return;
    }
    if (evt.type != TouchEventType::Touch || !evt.pressed) return;

    int16_t x = evt.x, y = evt.y;
    switch (currentScreen_) {
        case ScreenId::Home:     handleHomeTouch(x, y); break;
        case ScreenId::Bookshelf: handleShelfTouch(x, y); break;
        case ScreenId::Detail:    handleDetailTouch(x, y); break;
        case ScreenId::Reading:   handleReadingTouch(x, y); break;
        case ScreenId::Toc:       handleTocTouch(x, y); break;
        case ScreenId::FontSelect: handleFontTouch(x, y); break;
        case ScreenId::Settings:  handleSettingsTouch(x, y); break;
        default: break;
    }
}

void WereadApp::handleButton(const ButtonEvent& evt) {
    if (evt.id == ButtonId::Power && evt.action == ButtonAction::ShortPress) {
        enterSleep();
        return;
    }
    if (currentScreen_ == ScreenId::Reading) {
        if (evt.id == ButtonId::Up) {
            if (evt.action == ButtonAction::ShortPress) turnPage(-1);
        } else if (evt.id == ButtonId::Down) {
            if (evt.action == ButtonAction::ShortPress) turnPage(1);
        }
    } else if (currentScreen_ == ScreenId::Bookshelf) {
        if (evt.action == ButtonAction::ShortPress) {
            if (evt.id == ButtonId::Up) { shelfPage_--; renderBookshelf(true); }
            else if (evt.id == ButtonId::Down) { shelfPage_++; renderBookshelf(true); }
        }
    } else if (currentScreen_ == ScreenId::Toc) {
        if (evt.action == ButtonAction::ShortPress) {
            if (evt.id == ButtonId::Up) { tocPage_--; renderToc(); }
            else if (evt.id == ButtonId::Down) { tocPage_++; renderToc(); }
        }
    }
}

void WereadApp::handleShelfTouch(int16_t x, int16_t y) {
    // 返回箭头 → 主页
    if (x >= 28 && x < 76 && y >= 24 && y < 72) {
        navigateTo(ScreenId::Home);
        return;
    }
    // 底部翻页
    if (y > SCR_H - 70) {
        shelfPage_ += (x < SCR_W / 2) ? -1 : 1;
        renderBookshelf(true);
        return;
    }
    // 3列×2行网格 → 详情
    static const int16_t COL_X[3] = {28, 201, 374};
    static const int16_t ROW_Y[2] = {96, 357};
    static const int16_t COVER_W = 149;
    static const int16_t CELL_H = 185 + 4 + 44;  // 封面+间距+书名
    for (int r = 0; r < 2; r++) {
        if (y < ROW_Y[r] || y >= ROW_Y[r] + CELL_H) continue;
        for (int c = 0; c < 3; c++) {
            if (x < COL_X[c] || x >= COL_X[c] + COVER_W) continue;
            int idx = shelfPage_ * SHELF_PER_PAGE + r * 3 + c;
            if (idx < (int)shelf_.size()) {
                ESP_LOGI(TAG, "详情 %d. %s", idx + 1, shelf_[idx].title.c_str());
                openBookDetail(idx);
            }
            return;
        }
    }
}

void WereadApp::handleDetailTouch(int16_t x, int16_t y) {
    // 删除整本下载确认对话框（居中）：仅响应"删除/取消"，其余点击关闭
    if (detailDeletePending_) {
        int cx = SCR_W / 2;
        int boxY = SCR_H / 2 - 60;
        if (y >= boxY + 60 && y <= boxY + 104) {
            if (x >= cx - 140 && x <= cx - 10) {
                ESP_LOGI(TAG, "确认删除整本下载");
                detailDeletePending_ = false;
                if (SdCard::instance().ready()) {
                    Storage::instance().deleteBookCache(curBook_.bookId.c_str());
                }
                renderBookDetail();
                return;
            }
            if (x >= cx + 10 && x <= cx + 140) {
                ESP_LOGI(TAG, "取消删除");
                detailDeletePending_ = false;
                renderBookDetail();
                return;
            }
        }
        detailDeletePending_ = false;
        renderBookDetail();
        return;
    }
    // 返回书架
    if (y > SCR_H - 60 && x > SCR_W - 120) {
        renderBookshelf(false);
        return;
    }
    // 底部按钮
    if (y >= SCR_H - 130 && y <= SCR_H - 64) {
        if (x >= PAD - 2 && x <= PAD + 218) {
            ESP_LOGI(TAG, "继续阅读");
            continueReading();
            return;
        }
        if (x >= SCR_W - PAD - 220 && x <= SCR_W - PAD) {
            // 已下载 → 进入删除确认；未下载 → 触发下载
            bool dlDone = SdCard::instance().ready() &&
                          Storage::instance().bookDownloadDone(curBook_.bookId.c_str(), (int)chapters_.size());
            if (dlDone) {
                ESP_LOGI(TAG, "已下载，点击进入删除确认");
                detailDeletePending_ = true;
                renderBookDetail();
                return;
            }
            ESP_LOGI(TAG, "下载整本");
            downloadAll();
            return;
        }
    }
    // 插图开关
    if (y > SCR_H - 64 && y <= SCR_H - 6 && x >= PAD - 2 && x <= PAD + 218) {
        bool now = !Storage::instance().loadImgOff(curBook_.bookId.c_str());
        Storage::instance().saveImgOff(curBook_.bookId.c_str(), now);
        renderBookDetail();
        return;
    }
}

void WereadApp::handleReadingTouch(int16_t x, int16_t y) {
    // 顶部：左=目录，右=下一页
    if (y < 70) {
        if (x < 180) {
            ESP_LOGI(TAG, "目录");
            if (!chapters_.empty()) {
                tocPage_ = chapterIdx_ / std::max(1, (SCR_H - TOC_LIST_Y0 - 70) / (uiLineHeight() + 10));
                renderToc();
            }
        } else {
            turnPage(1);
        }
        return;
    }
    // 左下角版式
    if (y > SCR_H - 60 && x < 120) {
        ESP_LOGI(TAG, "版式设置");
        loadFontList();
        renderFontSelect();
        return;
    }
    // 右下角书架
    if (y > SCR_H - 60 && x > SCR_W - 120) {
        ESP_LOGI(TAG, "返回书架");
        if (SdCard::instance().ready()) Storage::instance().removeState();
        renderBookshelf(false);
        return;
    }
    // 左半=上页，右半=下页
    turnPage(x >= SCR_W / 2 ? 1 : -1);
}

void WereadApp::handleTocTouch(int16_t x, int16_t y) {
    if (y < 70) {
        backToReading();
        return;
    }
    if (y > SCR_H - 70) {
        int per = (SCR_H - TOC_LIST_Y0 - 70) / (uiLineHeight() + 10);
        int totalPages = std::max(1, ((int)chapters_.size() + per - 1) / per);
        tocPage_ += (x < SCR_W / 2) ? -1 : 1;
        if (tocPage_ < 0) tocPage_ = 0;
        if (tocPage_ >= totalPages) tocPage_ = totalPages - 1;
        renderToc();
        return;
    }
    if (y < TOC_LIST_Y0) return;
    int lh = uiLineHeight() + 10;
    int per = (SCR_H - TOC_LIST_Y0 - 70) / lh;
    int row = (y - TOC_LIST_Y0) / lh;
    int idx = tocPage_ * per + row;
    if (row < per && idx < (int)chapters_.size()) {
        ESP_LOGI(TAG, "目录选章 %d. %s", idx + 1, chapters_[idx].title.c_str());
        openChapter(idx);
    }
}

void WereadApp::handleFontTouch(int16_t x, int16_t y) {
    // × 关闭
    if (y < 60 && x > SCR_W - 60) {
        if (layoutDirty_) {
            applyLayout();
            layoutDirty_ = false;
        }
        backToReading();
        return;
    }
    if (y < 60) return;

    // 版式三行
    if (y >= 96 && y < 96 + 3 * LAYOUT_ROW_H) {
        int row = (y - 96) / LAYOUT_ROW_H;
        int* val = (row == 0) ? &lineSpacing_ : (row == 1) ? &paraGapMode_ : &paraIndent_;
        int cx = SCR_W - PAD - 3 * 100 - 2 * 8;
        for (int i = 0; i < 3; i++) {
            if (x >= cx && x < cx + 100) {
                if (*val != i) {
                    *val = i;
                    layoutDirty_ = true;
                    renderFontSelect();
                }
                return;
            }
            cx += 100 + 8;
        }
        return;
    }

    // 字体列表
    if (y < FONT_LIST_Y0) return;
    int row = (y - FONT_LIST_Y0) / FONT_ROW_H;
    if (row >= 0 && row < (int)fontList_.size()) {
        applyFont(row);
    }
}

void WereadApp::handleHomeTouch(int16_t x, int16_t y) {
    // 彩蛋显示中：任意点击一次即消失
    if (homeEasterShown_) {
        homeEasterShown_  = false;
        homeEasterClicks_ = 0;
        renderHome();
        return;
    }

    // 继续阅读卡片 (28, 64, 496, 216)
    if (x >= 28 && x < 524 && y >= 64 && y < 280) {
        if (curBook_.bookId.empty()) {
            return;  // 无阅读记录
        }
        // 需要 TOC 才能继续阅读
        if (chapters_.empty()) {
            screenMsg("加载目录...", curBook_.title.c_str());
            if (!fetchToc(curBook_.bookId)) {
                screenMsg("目录获取失败", curBook_.title.c_str());
                if (pollTouchDuringDelay(1500)) { renderHome(); return; }
                renderHome();
                return;
            }
        }
        ESP_LOGI(TAG, "主页 → 继续阅读: %s", curBook_.title.c_str());
        continueReading();
        return;
    }
    // 书封1 (28, 356, 140, 190)
    if (x >= 28 && x < 168 && y >= 356 && y < 546) {
        if (!shelf_.empty()) openBookDetail(0);
        return;
    }
    // 书封2 (184, 356, 140, 190)
    if (x >= 184 && x < 324 && y >= 356 && y < 546) {
        if (shelf_.size() > 1) openBookDetail(1);
        return;
    }
    // 书架标题 (28, 312, 100, 30) → 完整书架
    if (x >= 28 && x < 128 && y >= 312 && y < 342) {
        navigateTo(ScreenId::Bookshelf);
        return;
    }
    // 设置热区 (373, 356, 130, 156)
    if (x >= 373 && x < 503 && y >= 356 && y < 512) {
        navigateTo(ScreenId::Settings);
        return;
    }

    // 彩蛋：以上热区之外视为空白点击。累计 10 次显示彩蛋，再点一次消失。
    homeEasterClicks_++;
    if (homeEasterClicks_ >= 10) {
        homeEasterClicks_ = 0;
        homeEasterShown_  = true;
        renderHome();
    }
}

void WereadApp::handleSettingsTouch(int16_t x, int16_t y) {
    // 检查重置确认超时
    if (settingsResetPending_ &&
        (esp_timer_get_time() - settingsResetTime_) / 1000 > 5000000) {
        settingsResetPending_ = false;
    }

    // 返回箭头 (28, 24, 48, 48)
    if (x >= 28 && x < 76 && y >= 24 && y < 72) {
        settingsResetPending_ = false;
        navigateTo(ScreenId::Home);
        return;
    }

    // 条目1: 退出登录 (28, 100, 496, 64)
    if (x >= 28 && x < 524 && y >= 100 && y < 164) {
        ESP_LOGI(TAG, "设置 → 退出登录");
        screenMsg("退出登录中...", "清除账号信息");
        WR.setCookie("wr_vid", "");
        WR.setCookie("wr_skey", "");
        WR.setCookie("wr_rt", "");
        WR.cfg_api_key.clear();
        WR.saveCookiesToConfig();
        vTaskDelay(pdMS_TO_TICKS(1000));
        esp_restart();
        return;
    }
    // 条目2: WiFi 重连 (28, 180, 496, 64) — 用已保存配置重连
    if (x >= 28 && x < 524 && y >= 180 && y < 244) {
        ESP_LOGI(TAG, "设置 → WiFi 重连");
        settingsResetPending_ = false;
        screenMsg("正在重连 WiFi...", "扫描可见网络");
        if (WifiManager::instance().connectMulti() == WifiState::Connected) {
            wifiConnected_ = true;
            screenMsg("WiFi 已连接", "重启加载书架");
            vTaskDelay(pdMS_TO_TICKS(1500));
            esp_restart();
        } else {
            wifiConnected_ = false;
            screenMsg("重连失败", "仍是离线模式", "点按返回");
            vTaskDelay(pdMS_TO_TICKS(2000));
            renderSettings();
        }
        return;
    }
    // 条目3: WiFi 重置 (28, 260, 496, 64) — 清除配置重新配网
    if (x >= 28 && x < 524 && y >= 260 && y < 324) {
        ESP_LOGI(TAG, "设置 → WiFi 重置（清除配置重新配网）");
        settingsResetPending_ = false;
        // 清空 wifi_list 并保存
        AppConfig& cfg = Config::instance().mutableGet();
        cfg.wifiCount = 0;
        Config::instance().save();
        ESP_LOGI(TAG, "已清空 wifi_list，启动配网门户");
        startProvisioningAndWait();
        return;
    }
    // 条目4: 重置设备 (28, 340, 496, 64) — 双击确认
    if (x >= 28 && x < 524 && y >= 340 && y < 404) {
        if (!settingsResetPending_) {
            // 第一次点击：显示确认提示
            settingsResetPending_ = true;
            settingsResetTime_ = esp_timer_get_time();
            ESP_LOGI(TAG, "重置设备 — 等待二次确认");
            renderSettings();
        } else {
            // 第二次点击：执行重置
            settingsResetPending_ = false;
            ESP_LOGI(TAG, "重置设备 — 确认执行");
            screenMsg("重置设备中...", "清空所有数据");
            if (SdCard::instance().ready()) {
                SdCard::instance().remove(SD_CONFIG_PATH);
                SdCard::instance().remove(SD_STATE_PATH);
                SdCard::instance().remove(SD_PROGRESS_PATH);
                SdCard::instance().remove(SD_SHELF_PATH);
                SdCard::instance().remove(SD_IMGOFF_PATH);
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_restart();
        }
        return;
    }
    // 条目5: 检查更新 (28, 420, 496, 64)
    if (x >= 28 && x < 524 && y >= 420 && y < 484) {
        ESP_LOGI(TAG, "设置 → 检查更新");
        settingsResetPending_ = false;
        checkOtaUpdate();
        renderSettings();
        return;
    }
    // 条目6: 返回主页 (28, 500, 496, 64)
    if (x >= 28 && x < 524 && y >= 500 && y < 564) {
        settingsResetPending_ = false;
        navigateTo(ScreenId::Home);
        return;
    }
    // 点击空白区域取消重置确认
    if (settingsResetPending_) {
        settingsResetPending_ = false;
        renderSettings();
    }
}

// ================================================================
// OTA 升级
// ================================================================

// OTA 下载上下文（由 HTTP 事件回调写入到 OTA 分区）
struct OtaDlCtx {
    esp_ota_handle_t handle = 0;
    int total_written = 0;
    int content_length = 0;   // 来自 Content-Length 头；0=未知
    int last_pct = -1;        // 上次显示的百分比
    bool error = false;       // esp_ota_write 失败
    bool cancel = false;      // 用户点击取消
    Gslx680Touch* touch = nullptr;
    PortraitCanvas* cv = nullptr;
};

// OTA 固件下载 HTTP 事件回调：把数据流式写入 OTA 分区，每 10% 刷新进度
static esp_err_t otaHttpEventHandler_(esp_http_client_event_t* evt) {
    OtaDlCtx* ctx = (OtaDlCtx*)evt->user_data;
    if (!ctx) return ESP_OK;
    switch (evt->event_id) {
        case HTTP_EVENT_ON_DATA: {
            if (ctx->error || ctx->cancel || evt->data_len <= 0) break;

            // 检查用户是否点击取消
            if (ctx->touch) {
                TouchEvent tevt = ctx->touch->poll();
                if (tevt.type == TouchEventType::Touch && tevt.pressed) {
                    ctx->cancel = true;
                    ESP_LOGI(TAG, "OTA: 用户取消下载");
                    break;
                }
            }

            esp_err_t w = esp_ota_write(ctx->handle, evt->data, evt->data_len);
            if (w != ESP_OK) {
                ESP_LOGE(TAG, "OTA: esp_ota_write 失败: %s", esp_err_to_name(w));
                ctx->error = true;
            } else {
                ctx->total_written += evt->data_len;
                // 首包拿到 Content-Length（chunked 传输返回 -1，跳过百分比）
                if (ctx->content_length == 0) {
                    int64_t cl = esp_http_client_get_content_length(evt->client);
                    ctx->content_length = (cl > 0) ? (int)cl : 0;
                }
                if (ctx->content_length > 0 && ctx->cv &&
                    FontManager::instance().uiLoaded()) {
                    int pct = (ctx->total_written * 100) / ctx->content_length;
                    if (pct >= ctx->last_pct + 10) {
                        ctx->last_pct = pct;
                        char buf[32];
                        snprintf(buf, sizeof(buf), "下载中 %d%%", pct);
                        ctx->cv->fillRect(0, SCR_H - 100, SCR_W, 100, WHITE);
                        int tw = FontManager::instance().uiTextWidth(buf);
                        FontManager::instance().drawText(*ctx->cv,
                            (SCR_W - tw) / 2, SCR_H - 70, buf, 1, false);
                        // 底部小字提示可取消
                        const char* hint = "点按退出";
                        int hw = FontManager::instance().uiTextWidth(hint);
                        FontManager::instance().drawText(*ctx->cv,
                            (SCR_W - hw) / 2, SCR_H - 40, hint, 0, false);
                        ctx->cv->display(RefreshMode::Fast);
                    }
                }
            }
            break;
        }
        default:
            break;
    }
    return ESP_OK;
}

// 设置页入口：HEAD 检查固件版本（Last-Modified），与 NVS 记录对比，提示确认后升级
void WereadApp::checkOtaUpdate() {
    ESP_LOGI(TAG, "OTA: 检查更新（当前版本 " FW_VERSION ")");

    // 1. 离线模式无法检查
    if (offlineMode_ || !wifiConnected_) {
        screenMsg("需要联网检查更新", FW_VERSION, "请先连接 WiFi");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    // 2. 检查中
    screenMsg("检查更新中...", FW_VERSION);

    // 3. HEAD 请求拿 Last-Modified
    HttpResponse resp = WR.head(OTA_FIRMWARE_URL);
    if (!resp.ok()) {
        ESP_LOGW(TAG, "OTA: HEAD 失败 status=%d err=%s", resp.status, resp.error.c_str());
        char l1[64];
        snprintf(l1, sizeof(l1), "检查失败: %d", resp.status);
        screenMsg(l1, resp.error.c_str());
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    // 4. 解析 Last-Modified（headers map 为小写 key）
    auto it = resp.headers.find("last-modified");
    if (it == resp.headers.end() || it->second.empty()) {
        ESP_LOGW(TAG, "OTA: 服务器未返回 Last-Modified");
        screenMsg("已是最新版本", FW_VERSION);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }
    const std::string& lastModified = it->second;
    ESP_LOGI(TAG, "OTA: 服务器固件时间: %s", lastModified.c_str());

    // 5. 读取已存储的 last-modified（上次成功升级的固件时间）
    std::string stored;
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READONLY, &h) == ESP_OK) {
        char buf[128] = {0};
        size_t len = sizeof(buf);
        if (nvs_get_str(h, OTA_NVS_KEY, buf, &len) == ESP_OK) {
            stored = buf;
        }
        nvs_close(h);
    }

    // 6. 相同 → 已是最新
    if (!stored.empty() && stored == lastModified) {
        ESP_LOGI(TAG, "OTA: 已是最新版本");
        screenMsg("已是最新版本", FW_VERSION);
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }

    // 7. 发现新版本，提示确认
    screenMsg("发现新版本", lastModified.c_str(), "点按确认更新");

    // 8. 等待触控确认（30s 超时即取消）
    uint32_t elapsed = 0;
    const uint32_t step = 50;
    bool confirmed = false;
    while (elapsed < 30000) {
        TouchEvent evt = touch_->poll();
        if (evt.type == TouchEventType::Touch && evt.pressed) {
            confirmed = true;
            break;
        }
        ButtonEvent bevt;
        if (ButtonManager::instance().poll(&bevt, 0)) {
            confirmed = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }

    if (!confirmed) {
        ESP_LOGI(TAG, "OTA: 用户未确认（超时），取消更新");
        return;
    }

    // 9. 执行升级（成功时内部重启，不会返回）
    performOtaUpdate(OTA_FIRMWARE_URL, lastModified);
}

// 执行 OTA：下载固件 → 写入 OTA 分区 → 切换启动分区 → 重启
bool WereadApp::performOtaUpdate(const std::string& firmwareUrl, const std::string& lastModified) {
    ESP_LOGI(TAG, "OTA: 开始升级 %s", firmwareUrl.c_str());
    screenMsg("准备升级...", "下载固件中");

    // 1. 获取待写入分区
    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        ESP_LOGE(TAG, "OTA: 无法获取更新分区");
        screenMsg("升级失败", "无可用 OTA 分区");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }
    ESP_LOGI(TAG, "OTA: 写入分区 %s (0x%x bytes)", update_part->label, update_part->size);

    // 2. 开始 OTA 写入（顺序写）
    esp_ota_handle_t update_handle = 0;
    esp_err_t err = esp_ota_begin(update_part, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_begin 失败: %s", esp_err_to_name(err));
        screenMsg("升级失败", "ota_begin 错误");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }

    // 3. 独立 HTTP client 下载固件（不走 WereadClient 会话，异 host）
    OtaDlCtx ctx;
    ctx.handle = update_handle;
    ctx.cv = canvas_;
    ctx.touch = touch_;

    esp_http_client_config_t config = {};
    config.url = firmwareUrl.c_str();
    config.cert_pem = NULL;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = 60000;
    config.buffer_size = 1024;
    config.buffer_size_tx = 1024;
    config.event_handler = otaHttpEventHandler_;
    config.user_data = &ctx;

    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        ESP_LOGE(TAG, "OTA: esp_http_client_init 失败");
        esp_ota_abort(update_handle);
        screenMsg("升级失败", "http init 错误");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }

    esp_err_t perf = esp_http_client_perform(client);
    int status = (perf == ESP_OK) ? esp_http_client_get_status_code(client) : -1;
    int64_t got_len = (perf == ESP_OK) ? esp_http_client_get_content_length(client) : -1;
    esp_http_client_cleanup(client);

    ESP_LOGI(TAG, "OTA: 下载结果 perf=%s status=%d written=%d content-length=%lld err=%d",
             esp_err_to_name(perf), status, ctx.total_written,
             (long long)got_len, (int)ctx.error);

    // 4. 用户取消
    if (ctx.cancel) {
        ESP_LOGI(TAG, "OTA: 用户已取消下载");
        esp_ota_abort(update_handle);
        screenMsg("已取消升级", "设备未受影响");
        vTaskDelay(pdMS_TO_TICKS(1500));
        return false;
    }

    // 5. 下载失败处理
    if (perf != ESP_OK || status < 200 || status >= 300 || ctx.error) {
        ESP_LOGE(TAG, "OTA: 下载失败");
        esp_ota_abort(update_handle);
        screenMsg("升级失败", "固件下载错误");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }
    if (got_len > 0 && ctx.total_written < (int)got_len) {
        ESP_LOGE(TAG, "OTA: 固件不完整 %d / %lld", ctx.total_written, (long long)got_len);
        esp_ota_abort(update_handle);
        screenMsg("升级失败", "固件不完整");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }

    // 5. 结束写入并校验
    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_end 失败: %s", esp_err_to_name(err));
        screenMsg("升级失败", "ota_end 错误");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }

    // 6. 切换启动分区
    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA: esp_ota_set_boot_partition 失败: %s", esp_err_to_name(err));
        screenMsg("升级失败", "set_boot 错误");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return false;
    }

    ESP_LOGI(TAG, "OTA: 升级完成，准备重启");
    screenMsg("升级完成，重启中...");

    // 7. 保存 last-modified 到 NVS（下次启动后据此判断是否最新）
    nvs_handle_t h;
    if (nvs_open(OTA_NVS_NAMESPACE, NVS_READWRITE, &h) == ESP_OK) {
        if (nvs_set_str(h, OTA_NVS_KEY, lastModified.c_str()) == ESP_OK) {
            nvs_commit(h);
            ESP_LOGI(TAG, "OTA: last-modified 已保存到 NVS");
        }
        nvs_close(h);
    }

    // 8. 重启
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return true;  // 不可达（esp_restart 不会返回）
}

// ================================================================
// 事件循环
// ================================================================

void WereadApp::eventLoop() {
    ESP_LOGI(TAG, "进入事件循环");
    // 主任务（事件循环）优先级提到最高档：翻页渲染/刷新/等待 BUSY 全程不可被
    // 后台任务（预取、WiFi 重连、串口调试等）抢占，确保每次翻页刷新可靠完成
    vTaskPrioritySet(NULL, configMAX_PRIORITIES - 1);
    int64_t lastActivity = esp_timer_get_time() / 1000;
    int64_t lastHousekeeping = 0;

    for (;;) {
        bool activity = false;

        // 触控
        TouchEvent tevt = touch_->poll();
        if (tevt.type != TouchEventType::None) {
            activity = true;
            handleTouch(tevt);
        }

        // 按键
        ButtonEvent bevt;
        if (ButtonManager::instance().poll(&bevt, 0)) {
            activity = true;
            handleButton(bevt);
        }

        if (activity) lastActivity = esp_timer_get_time() / 1000;

        // 闲置自动休眠（充电中不休眠）
        int64_t now = esp_timer_get_time() / 1000;
        if (now - lastActivity > AUTO_SLEEP_MS) {
            BatteryInfo bat = PowerManager::instance().readBattery();
            if (!bat.charging) {
                enterSleep();
            }
        }

        // 定期 housekeeping（5s）
        if (now - lastHousekeeping > 5000) {
            lastHousekeeping = now;
            weread_api::housekeeping();
            WR.housekeeping();
            // cookie 落盘
            if (WR.cookiesDirty()) {
                WR.saveCookiesToConfig();
                WR.clearCookiesDirty();
            }
            // 更新 WiFi 状态
            wifiConnected_ = WifiManager::instance().isConnected();
        }

        vTaskDelay(pdMS_TO_TICKS(8));  // 8ms 轮询（同触控驱动频率）
    }
}

// ================================================================
// 公共接口
// ================================================================

void WereadApp::navigateTo(ScreenId screen) {
    previousScreen_ = currentScreen_;
    currentScreen_ = screen;
    switch (screen) {
        case ScreenId::Home:     renderHome(); break;
        case ScreenId::Bookshelf: renderBookshelf(false); break;
        case ScreenId::Detail:    renderBookDetail(); break;
        case ScreenId::Reading:   renderReadingPage(RefreshMode::Full, true); break;
        case ScreenId::Toc:       renderToc(); break;
        case ScreenId::FontSelect: renderFontSelect(); break;
        case ScreenId::Settings:  renderSettings(); break;
        default: break;
    }
}

void WereadApp::goBack() {
    navigateTo(previousScreen_);
}

void WereadApp::refreshDisplay(RefreshMode mode) {
    switch (currentScreen_) {
        case ScreenId::Home:     renderHome(); break;
        case ScreenId::Bookshelf: renderBookshelf(true); break;
        case ScreenId::Detail:    renderBookDetail(); break;
        case ScreenId::Reading:   renderReadingPage(mode, true); break;
        case ScreenId::Toc:       renderToc(); break;
        case ScreenId::FontSelect: renderFontSelect(); break;
        case ScreenId::Settings:  renderSettings(); break;
        default: break;
    }
}

// ================================================================
// 串口调试命令（Task 17）
// ================================================================

// 串口命令回调跳板：CommandHandler 是普通函数指针，无法直接绑定成员函数，
// 故用一组静态跳板转发到 WereadApp 单例的对应方法。
namespace {
void scb_next(const char*)    { WereadApp::instance().serialNextPage(); }
void scb_prev(const char*)    { WereadApp::instance().serialPrevPage(); }
void scb_shelf(const char*)   { WereadApp::instance().serialShowShelf(); }
void scb_snext(const char*)   { WereadApp::instance().serialShelfNext(); }
void scb_sprev(const char*)   { WereadApp::instance().serialShelfPrev(); }
void scb_toc(const char*)     { WereadApp::instance().serialShowToc(); }
void scb_tnext(const char*)   { WereadApp::instance().serialTocNext(); }
void scb_tprev(const char*)   { WereadApp::instance().serialTocPrev(); }
void scb_tjump(const char* a) { WereadApp::instance().serialJumpChapter(a); }
void scb_dl(const char*)      { WereadApp::instance().serialDownloadAll(); }
void scb_wifi(const char*)    { WereadApp::instance().serialStartProvisioning(); }
void scb_login(const char*)   { WereadApp::instance().serialStartLogin(); }
void scb_cookie(const char*)  { WereadApp::instance().serialPrintCookie(); }
void scb_fonts(const char*)   { WereadApp::instance().serialPrintFonts(); }
void scb_books(const char*)   { WereadApp::instance().serialPrintBooks(); }
void scb_usb(const char*)     { WereadApp::instance().serialUsbDiag(); }
void scb_imgoff(const char*)  { WereadApp::instance().serialToggleImgOff(); }
void scb_cfg(const char* a)   { WereadApp::instance().serialWriteConfig(a); }
void scb_time(const char*)    { WereadApp::instance().serialPrintTime(); }
void scb_ds(const char* a)    { WereadApp::instance().serialDeepSleep(a); }
void scb_help(const char*)    { WereadApp::instance().handleSerialCommand("help"); }
}  // namespace

void WereadApp::registerSerialCommands() {
    auto& sd = SerialDebug::instance();
    sd.registerCommand("n",      scb_next);
    sd.registerCommand("p",      scb_prev);
    sd.registerCommand("s",      scb_shelf);
    sd.registerCommand("sp",     scb_snext);
    sd.registerCommand("sm",     scb_sprev);
    sd.registerCommand("t",      scb_toc);
    sd.registerCommand("tp",     scb_tnext);
    sd.registerCommand("tm",     scb_tprev);
    sd.registerCommand("ts",     scb_tjump);
    sd.registerCommand("dl",     scb_dl);
    sd.registerCommand("wifi",   scb_wifi);
    sd.registerCommand("login",  scb_login);
    sd.registerCommand("cookie", scb_cookie);
    sd.registerCommand("fonts",  scb_fonts);
    sd.registerCommand("books",  scb_books);
    sd.registerCommand("usb",    scb_usb);
    sd.registerCommand("imgoff", scb_imgoff);
    sd.registerCommand("cfg",    scb_cfg);
    sd.registerCommand("time",   scb_time);
    sd.registerCommand("ds",     scb_ds);
    sd.registerCommand("help",   scb_help);
    sd.registerCommand("?",      scb_help);
    ESP_LOGI(TAG, "已注册 %d 条串口命令", 22);
}

// 未匹配注册命令的行（N / N:C / help / 未知）走此入口
void WereadApp::handleSerialCommand(const char* line) {
    if (!line || !line[0]) return;
    while (*line == ' ' || *line == '\t') line++;
    if (!*line) return;

    // help / ?
    if (strcasecmp(line, "help") == 0 || strcmp(line, "?") == 0) {
        printf("=== 串口命令 ===\n");
        printf("n / p        下一页 / 上一页\n");
        printf("s / sp / sm  书架 / 下一页 / 上一页\n");
        printf("t / tp / tm  目录 / 下一页 / 上一页\n");
        printf("ts N         跳转第 N 章\n");
        printf("N            打开第 N 本书详情\n");
        printf("N:C          直读第 N 本第 C 章\n");
        printf("dl           下载整本\n");
        printf("wifi         配网门户\n");
        printf("login        扫码登录\n");
        printf("cookie       导出 cookie\n");
        printf("fonts        字体列表\n");
        printf("books        书架列表\n");
        printf("usb          充电诊断\n");
        printf("imgoff       切换插图开关\n");
        printf("cfg {json}   写配置\n");
        printf("time         当前时间\n");
        printf("ds <3-60>    深睡 N 秒\n");
        return;
    }

    // N 或 N:C（N 为书架序号，1 起）
    if (isdigit((unsigned char)line[0])) {
        const char* colon = strchr(line, ':');
        int n = atoi(line);
        if (n < 1 || n > (int)shelf_.size()) {
            printf("[?] 书号 %d 越界 (1-%d)\n", n, (int)shelf_.size());
            return;
        }
        if (colon) {
            serialOpenBook(line);
        } else {
            openBookDetail(n - 1);
        }
        return;
    }

    printf("[?] 未知命令: %s (输入 help 查看命令)\n", line);
}

// ---- 目录相关 ----
void WereadApp::serialShowToc() {
    if (chapters_.empty()) { printf("[t] 无目录\n"); return; }
    renderToc();
}

void WereadApp::serialTocNext() { tocPage_++; renderToc(); }
void WereadApp::serialTocPrev() { tocPage_--; renderToc(); }

void WereadApp::serialJumpChapter(const char* args) {
    if (chapters_.empty()) { printf("[ts] 无目录\n"); return; }
    if (!args || !args[0]) { printf("[ts] 用法: ts N (1 起)\n"); return; }
    int n = atoi(args);
    if (n < 1 || n > (int)chapters_.size()) {
        printf("[ts] 章节 %d 越界 (1-%d)\n", n, (int)chapters_.size());
        return;
    }
    printf("[ts] 跳转第 %d 章: %s\n", n, chapters_[n - 1].title.c_str());
    openChapter(n - 1);
}

// ---- 直读 N:C ----
void WereadApp::serialOpenBook(const char* args) {
    if (!args || !args[0]) { printf("[?] 用法: N:C (如 3:5)\n"); return; }
    if (shelf_.empty()) { printf("[?] 书架为空\n"); return; }
    int n = atoi(args);
    int c = 1;
    const char* colon = strchr(args, ':');
    if (colon) c = atoi(colon + 1);
    if (n < 1 || n > (int)shelf_.size()) {
        printf("[?] 书号 %d 越界 (1-%d)\n", n, (int)shelf_.size());
        return;
    }
    openBookDirect(n - 1, c);
}

// 串口直读（跳过详情页）：打开书架第 shelfIdx 本，直读第 chIdx1Based 章
void WereadApp::openBookDirect(int shelfIdx, int chIdx1Based) {
    if (shelfIdx < 0 || shelfIdx >= (int)shelf_.size()) return;
    curBook_ = shelf_[shelfIdx];
    bookIdx_ = shelfIdx;
    screenMsg("拉取目录...", curBook_.title.c_str());
    if (!fetchToc(curBook_.bookId)) {
        printf("[错误] 目录获取失败: %s\n", curBook_.title.c_str());
        screenMsg("目录获取失败", curBook_.title.c_str());
        return;
    }
    printf("[目录] 《%s》 %d 章 format=%s\n",
           curBook_.title.c_str(), (int)chapters_.size(), bookFormat_.c_str());
    if (chIdx1Based < 1 || chIdx1Based > (int)chapters_.size()) chIdx1Based = 1;
    openChapter(chIdx1Based - 1);
}

// ---- 下载整本 ----
void WereadApp::serialDownloadAll() { downloadAll(); }

// 下载整本：逐章拉取正文并写入 SD 缓存，跳过已缓存章节，显示进度，点按取消
void WereadApp::downloadAll() {
    if (curBook_.bookId.empty() || chapters_.empty()) {
        printf("[dl] 无当前书或目录\n");
        return;
    }
    // 目录必然在线（downloadAll 仅在线可用），顺手写目录缓存，
    // 保证"下载过整本"的书离线也能打开（离线时 fetchToc 直接读 toc.json）
    if (SdCard::instance().ready()) {
        Storage::instance().saveTocCache(curBook_.bookId.c_str(), chapters_, bookFormat_.c_str());
    }
    int total = (int)chapters_.size(), done = 0, fetched = 0;
    printf("[dl] 开始下载《%s》共 %d 章\n", curBook_.title.c_str(), total);
    screenMsg("下载整本...", curBook_.title.c_str());
    weread_api::g_net_cancel = false;

    // 下载时是否加载图片：复用详情页「插图：开/关」开关，关则跳过图片块、省 SD 空间
    bool skipImg = SdCard::instance().ready() &&
                   Storage::instance().loadImgOff(curBook_.bookId.c_str());
    if (skipImg) printf("[dl] 插图已关闭，跳过图片下载\n");

    // 等待手指抬起：下载由按钮的按压事件触发，此刻手指仍按着，
    // 若直接 poll 会把这同一个按压当成"取消"，导致一点就取消。
    // 等到检测到抬起（pressed=false）或无触控再进入下载循环。
    {
        int64_t waitT0 = esp_timer_get_time();
        while (esp_timer_get_time() - waitT0 < 2000 * 1000) {
            TouchEvent ev = touch_->poll();
            if (ev.type == TouchEventType::Touch && !ev.pressed) break;  // 已抬起
            if (ev.type == TouchEventType::None) break;                  // 无触控
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }

    for (int i = 0; i < total; i++) {
        // 点按取消（触控队列非空即取消）
        TouchEvent tevt = touch_->poll();
        if (tevt.type != TouchEventType::None) {
            weread_api::g_net_cancel = true;
            printf("[dl] 已取消 (已完成 %d/%d)\n", done, total);
            screenMsg("下载已取消", curBook_.title.c_str(),
                      (std::string("已完成 ") + std::to_string(done) + "/" + std::to_string(total)).c_str());
            renderBookDetail();
            return;
        }

        weread_api::ChapterEntry& ch = chapters_[i];
        // 已缓存章节跳过
        if (SdCard::instance().ready() &&
            Storage::instance().chapterCacheExists(curBook_.bookId.c_str(), ch.chapterUid.c_str())) {
            done++;
            continue;
        }

        std::vector<weread_api::ContentBlock> blocks;
        std::string err;
        if (weread_api::getChapterBlocks(curBook_.bookId, ch, blocks, err) && !blocks.empty()) {
            if (SdCard::instance().ready()) {
                if (skipImg) {
                    // 插图关闭：过滤掉图片块，只缓存文本
                    std::vector<weread_api::ContentBlock> filtered;
                    for (const auto& b : blocks) {
                        if (!b.is_image) filtered.push_back(b);
                    }
                    if (!filtered.empty()) {
                        Storage::instance().chapterCacheSave(curBook_.bookId.c_str(),
                                                             ch.chapterUid.c_str(), filtered);
                    }
                } else {
                    Storage::instance().chapterCacheSave(curBook_.bookId.c_str(),
                                                         ch.chapterUid.c_str(), blocks);
                }
            }
            done++; fetched++;
            printf("[dl] %d/%d %s OK (%u 块)\n",
                   i + 1, total, ch.title.c_str(), (unsigned)blocks.size());
        } else {
            // 会员章节（e_0）或拉取失败：跳过不中断整体流程
            printf("[dl] %d/%d %s 失败: %s\n",
                   i + 1, total, ch.title.c_str(), err.c_str());
        }
        // 每章更新进度
        screenMsg("下载整本...", curBook_.title.c_str(),
                  (std::string("已完成 ") + std::to_string(done) + "/" + std::to_string(total)).c_str());
    }

    printf("[dl] 完成 %d/%d 新拉 %d\n", done, total, fetched);
    // 全量完成才打标：部分完成不标（补漏时按钮还可点）
    if (done == total && total > 0 && SdCard::instance().ready()) {
        Storage::instance().bookDownloadDoneMark(curBook_.bookId.c_str(), total);
        printf("[dl] 已标记 done.flag\n");
    }
    screenMsg("下载完成", curBook_.title.c_str(),
              (std::string(std::to_string(done)) + "/" + std::to_string(total) + " 章").c_str());
    renderBookDetail();
}

// ---- 配网 / 登录 ----
void WereadApp::serialStartProvisioning() {
    printf("[wifi] 启动配网门户\n");
    startProvisioningAndWait();
}

// 启动配网门户并阻塞等待：配网成功后重启，超时(3 分钟)或失败则停止门户返回
void WereadApp::startProvisioningAndWait() {
    renderProvisionGuide();
    if (ProvisionPortal::instance().start() != ESP_OK) {
        screenMsg("配网启动失败", "点按继续");
        vTaskDelay(pdMS_TO_TICKS(2000));
        return;
    }
    // 阻塞等待配网成功（与 initWifi 首次配网流程一致）
    // 配网成功后 WifiManager 会收到 GOT_IP 进入 Connected 状态
    int64_t deadline = esp_timer_get_time() + 180 * 1000 * 1000LL;  // 3 分钟超时
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        if (WifiManager::instance().isConnected()) {
            ESP_LOGI(TAG, "配网成功，重启");
            screenMsg("配网成功", "重启中...");
            vTaskDelay(pdMS_TO_TICKS(1500));
            ProvisionPortal::instance().stop();
            esp_restart();
        }
    }
    // 超时：停止门户，返回事件循环
    ESP_LOGW(TAG, "配网超时，退出配网模式");
    ProvisionPortal::instance().stop();
    screenMsg("配网超时", "已退出配网模式");
    vTaskDelay(pdMS_TO_TICKS(1500));
}

void WereadApp::serialStartLogin() {
    printf("[login] 启动扫码登录\n");
    if (doQrLogin()) {
        printf("[login] 登录成功，重载书架\n");
        shelf_.clear();
        std::string err;
        weread_api::getBookshelf(shelf_, err);
        renderHome();
    } else {
        printf("[login] 登录未完成\n");
    }
}

// ---- 诊断输出 ----
void WereadApp::serialPrintCookie() {
    printf("[cookie] wr_vid=%s wr_skey=%s wr_rt=%s\n",
           WR.getCookie("wr_vid").c_str(),
           WR.getCookie("wr_skey").c_str(),
           WR.getCookie("wr_rt").c_str());
    printf("[cookie] api_key=%s valid=%d dirty=%d\n",
           WR.cfg_api_key.c_str(), (int)WR.hasValidCookie(), (int)WR.cookiesDirty());
}

void WereadApp::serialPrintFonts() {
    auto& fm = FontManager::instance();
    printf("[fonts] Body path=%s format=%d loaded=%d h=%d\n",
           bodyFontPath_.c_str(), (int)fm.bodyFormat(), (int)fm.bodyLoaded(), fm.bodyFontHeight());
    if (fm.uiLoaded()) {
        printf("[fonts] UI   loaded=%d h=%d format=%d\n",
               (int)fm.uiLoaded(), fm.uiFontHeight(), (int)fm.uiFormat());
    }

    if (!SdCard::instance().ready()) { printf("[fonts] SD 未就绪\n"); return; }

    // 目录项数组 ~17KB，放堆上避免爆主任务栈
    SdCard::DirEntry* entries =
        (SdCard::DirEntry*)calloc(64, sizeof(SdCard::DirEntry));
    if (!entries) return;
    int n = SdCard::instance().listDir("/", entries, 64);
    for (int i = 0; i < n; i++) {
        if (entries[i].isDir) continue;
        std::string name = entries[i].name;
        if (name.rfind("._", 0) == 0) continue;  // 跳过 macOS 元数据
        if (name.find(".bin") == std::string::npos &&
            name.find(".eefont") == std::string::npos) continue;

        std::string path = "/" + name;
        FILE* f = SdCard::instance().open(path.c_str(), "rb");
        if (!f) { printf("  %s (打开失败)\n", name.c_str()); continue; }
        uint8_t hdr[8] = {};
        size_t rn = fread(hdr, 1, sizeof(hdr), f);
        fclose(f);

        if (rn >= 6 && (memcmp(hdr, "EEFONT", 6) == 0 || memcmp(hdr, "CPFONT", 6) == 0)) {
            printf("  %s  CPFONTv4 大小=%u\n", name.c_str(), (unsigned)entries[i].size);
        } else if (rn >= 5 && name.find(".bin") != std::string::npos) {
            uint32_t cc = hdr[0] | (hdr[1] << 8) | (hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
            printf("  %s  EDCBook 字符数=%lu 字高=%d 大小=%u\n",
                   name.c_str(), (unsigned long)cc, hdr[4], (unsigned)entries[i].size);
        } else {
            printf("  %s  未知格式 大小=%u\n", name.c_str(), (unsigned)entries[i].size);
        }
    }
    free(entries);
}

void WereadApp::serialPrintBooks() {
    printf("[books] 共 %d 本:\n", (int)shelf_.size());
    for (size_t i = 0; i < shelf_.size(); i++) {
        printf("  %d. %s [%s] %d%%\n",
               (int)(i + 1), shelf_[i].title.c_str(),
               shelf_[i].bookId.c_str(), shelf_[i].progress);
    }
}

void WereadApp::serialUsbDiag() {
    BatteryInfo bat = PowerManager::instance().readBattery();
    printf("[usb] 充电=%d 电池=%umV %u%%\n",
           (int)bat.charging, bat.millivolts, bat.percent);
}

void WereadApp::serialToggleImgOff() {
    if (curBook_.bookId.empty()) { printf("[imgoff] 无当前书\n"); return; }
    bool now = !Storage::instance().loadImgOff(curBook_.bookId.c_str());
    Storage::instance().saveImgOff(curBook_.bookId.c_str(), now);
    printf("[imgoff] %s → %s\n", curBook_.title.c_str(), now ? "关" : "开");
    if (currentScreen_ == ScreenId::Reading && !pages_.empty()) {
        renderReadingPage(RefreshMode::Full, true);
    }
}

void WereadApp::serialWriteConfig(const char* json) {
    if (!json || !json[0]) { printf("[cfg] 用法: CFG {json}\n"); return; }
    if (!SdCard::instance().ready()) { printf("[cfg] SD 未就绪\n"); return; }
    std::string data(json);
    bool ok = SdCard::instance().writeFile(SD_CONFIG_PATH,
                                            (const uint8_t*)data.c_str(), data.size());
    if (ok) {
        printf("[cfg] 已写入 %u 字节\n", (unsigned)data.size());
        Config::instance().load();
        printf("[cfg] 配置已重载\n");
    } else {
        printf("[cfg] 写入失败\n");
    }
}

void WereadApp::serialPrintTime() {
    time_t now = time(nullptr);
    printf("[time] unix=%lld\n", (long long)now);
}

void WereadApp::serialDeepSleep(const char* args) {
    uint32_t secs = 0;
    if (args && args[0]) secs = (uint32_t)atoi(args);
    if (secs == 0) { printf("[ds] 用法: ds <3-60>\n"); return; }
    printf("[ds] %lus 后进入深睡\n", (unsigned long)secs);
    saveProgress();
    SleepManager::instance().enterDeepSleep(secs);
    // enterDeepSleep 不会返回
}

}  // namespace eego
