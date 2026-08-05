// weread_api.h — 微信读书接口封装
// 纯 ESP-IDF：std::string 替代 Arduino String，cJSON 替代 ArduinoJson。
#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace weread_api {

// 书架条目
struct BookEntry {
    std::string bookId;
    std::string title;
    std::string author;
    std::string cover;
    std::string format;    // "epub" 或 "txt"（可能为空）
    int progress = 0;      // 阅读进度百分比（来自 shelf/sync 的 bookProgress[]）
    std::string progressChapterUid;   // 进度所在章节 chapterUid
    uint32_t readUpdateTime = 0;      // 最近阅读时间 unix 秒
};

struct ChapterEntry {
    std::string chapterUid;
    std::string title;
    int chapterIdx = 0;
    int wordCount = 0;
    bool paid = false;
    std::string tar;       // 本章资源 tar 包 URL（可能为空）
};

// 书籍详情（weread.qq.com/web/book/info）
struct BookDetail {
    std::string title;
    std::string author;
    std::string intro;
    std::string authorIntro;  // web/book/info 不返回，留空
    std::string cover;
    std::string publisher;
};

// 热门评论（weread.qq.com/web/review/list?listType=3）
struct Review {
    std::string nick;
    std::string content;
    int likes = 0;
};

// 章节内容块：图文分块阅读用
struct ContentBlock {
    bool is_image = false;
    std::string text;
    uint8_t style = 0;  // 0=正文，1..4 = <h1>..<h4> 标题
};

// 书架（weread.qq.com/web/shelf/sync，cookie）
bool getBookshelf(std::vector<BookEntry>& out, std::string& err);

// 书籍信息（i.weread.qq.com/book/info，cookie）
bool getBookInfo(const std::string& bookId, BookEntry& out, std::string& err);

// 目录章节列表（weread.qq.com/web/book/chapterInfos，cookie + 自动格式探测）
bool getChapterInfos(const std::string& bookId, std::vector<ChapterEntry>& out,
                     std::string& format_out, std::string& err);

// 从 reader HTML 中提取 psvts（正文请求必需）
std::string extractPsvts(const char* html, size_t len);
std::string extractPsvts(const std::string& readerHtml);

// 拉取一章纯文本正文（自动探测 epub/txt；txt 直出，epub 去标签）
bool getChapterText(const std::string& bookId, const ChapterEntry& ch,
                    std::string& text_out, std::string& err);

// 上传阅读进度（POST weread.qq.com/web/book/read）
bool uploadProgress(const std::string& bookId, const std::string& chapterUid,
                    int chapterIdx, int progressPercent, std::string& err);

// 章节拉取进度回调（main.cpp 注入显示进度）
extern void (*chapter_stage_cb)(const char* stage, int cur, int total);

// 全局取消标志
extern volatile bool g_net_cancel;

// 会话闲置关闭（主循环周期调用）
void housekeeping();

// 书籍详情（weread.qq.com/web/book/info，cookie）
bool getBookDetail(const std::string& bookId, BookDetail& out, std::string& err);

// 热门评论（weread.qq.com/web/review/list?listType=3，cookie）
bool getHotReviews(const std::string& bookId, std::vector<Review>& out,
                   int count, std::string& err);

// 拉取一章并按文档顺序切图文块。
bool getChapterBlocks(const std::string& bookId, const ChapterEntry& ch,
                      std::vector<ContentBlock>& out, std::string& err);

} // namespace weread_api
