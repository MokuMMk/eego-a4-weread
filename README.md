# EEGO A4 微信读书墨水屏阅读器

把微信读书搬进 **EEGO A4**（ESP32-S3 + 4.7 寸墨水屏，540×960 竖屏）的独立阅读器固件。
配网、扫码登录、选书、阅读、进度同步、整本下载、离线阅读，全程不需要电脑。

![platform](https://img.shields.io/badge/platform-EEGO%20A4%20(ESP32--S3)-blue) ![framework](https://img.shields.io/badge/framework-ESP--IDF%20v5.5-green) ![license](https://img.shields.io/badge/license-GPL--3.0-orange) ![status](https://img.shields.io/badge/status-真机可用-brightgreen)

---

## 功能一览

- **WiFi 多账号**：config.json 保存多个网络（家里/办公室），扫描到可见 AP 自动连接，配网门户自动追加
- **配网门户**：无配置或连接失败时自动开热点，手机/电脑网页选 WiFi 输密码，失败显示中文原因
- **扫码登录**：cookie 过期自动弹微信读书二维码，微信扫码即登录；令牌自动续期（wr_rt → wr_skey），登录一次长期免维护
- **书架**：封面 5 本/页 + 最近阅读排序 + 真实阅读进度百分比
- **书籍详情**：封面/标题/作者/出版社/简介/热门评论；继续阅读、**下载整本**（进度条 + 可取消）、插图开关
- **正文阅读**：图文混排（JPEG/PNG 灰度上屏、透明 PNG 双线性 alpha 混合）、原书标题样式、首行缩进、段间距、版心居中
- **快刷翻页**：epd_fast 异步推送，按需全刷清残影；字形缓存让翻页保持在亚秒级
- **目录屏**：章节名入口，翻页/选章/返回
- **进度同步**：本地 SD 页级进度 + 上传微信读书服务器，手机端互通
- **后台预取**：读完一章自动预拉下一章（正文 + 插图）
- **整本下载与离线阅读**：全书正文缓存到 SD，章节级缓存（B7 头 .blk 格式）；离线模式下直接读本地缓存，不再请求注定失败的目录
- **休眠**：浅睡（轻触恢复）+ 24 小时无活动自动转深睡（电源键 EXT0 唤醒）；休眠期间断 WiFi 省电
- **版式设置**：行距/段距/首行缩进可调，持久化
- **字体**：SD 卡任意字体（EDCBook `.bin` / CPFONT `.eefont`），阅读页实时切换、列表逐字体预览

## 性能与预期（请先读）

ESP32-S3 是**单片机级算力**（240MHz 双核、几 MB 内存），和手机/平板/reMarkable 这类
跑 Linux 的设备**不在一个量级**。有些场景会有可感知的等待，这是硬件天花板，不是异常：

- **首次联网内容**（拉新书目录/详情/正文、插图、下载整本）受 TLS 握手成本影响，单次请求 1-2 秒，
  整本下载每分钟约 10-15 章
- **首次打开的章节**要解密+分页，比之后慢；插图多的章节首次还要逐张下载插图
- **流畅的部分也不含糊**：已缓存书籍**离线阅读、翻页（亚秒级推送）、目录、字体切换**和高端阅读器一样顺

日常阅读（翻页/目录/字体）已经够用，下载类的慢一次之后永久缓存。

## 硬件与 SD 卡准备

- EEGO A4 设备（ESP32-S3 + 4.7" 墨水屏），Type-C 供电
- **TF 卡一张，根目录必须放置字体文件**（`.eefont` 或 `.bin`，文件名任意）：
  - UI 字体与正文字体都从 SD 卡根目录读取
  - **建议使用 12px 大小字体**（兼顾清晰度与渲染速度）
  - 仓库已内置字体，按需拷贝到 SD 卡根目录即可：
    - `HarmonyOS_Sans_SC/` — CPFONT `.eefont`（12 / 14 / 16 / 18px）
    - `eego-a4-weread-fonts/` — EDCBook `.bin`（16 ~ 36px）

## 快速开始（编译与烧录）

依赖：[ESP-IDF v5.x](https://docs.espressif.com/projects/esp-idf/)（开发时使用 v5.5.1）。

```powershell
# 1. 加载 ESP-IDF 环境（以 v5.5.1 为例）
. D:\Espressif\frameworks\esp-idf-v5.5.1\export.ps1

# 2. 编译（分区表/PSRAM/WiFi 等配置由 sdkconfig.defaults 提供）
idf.py build

# 3. 烧录（端口按实际 COM 号修改）
idf.py -p COM6 flash
```

生成的固件位于 `build/papers3-weread-eegoa4.bin`。

### 网页一键刷机

`web_flasher/` 提供免命令行刷机页（Web Serial + esptool-js）：连接设备 → 自动下载固件 → 自动烧录 → 自动重启。
自托管后浏览器打开 `index.html` 即可，注意需 Chrome/Edge 89+。

## 首次使用（零电脑）

1. 烧录固件后开机，显示首屏
2. 设备自动开热点，屏幕显示提示
3. 手机/电脑连接热点（自动弹配网页，或浏览器开 `192.168.4.1`）
4. 扫描 → 选 WiFi → 输密码 → 连接成功自动重启
5. 重启后连 WiFi → 弹出微信读书二维码 → 微信扫码 → 进入书架

以后开机即用：cookie 失效会自动续期，续不动才再扫码。

## 操作说明

| 页面 | 操作 |
|------|------|
| 书架 | 点书进详情；底部左/右半翻页 |
| 详情 | 继续阅读 / 下载整本（进度条，点按取消）/ 插图开关 |
| 阅读页 | 右半屏下一页、左半屏上一页；左上角章节名进目录；底部「Aa」版式、「书架」返回 |
| 目录屏 | 点章节跳转；底部翻页；顶部返回 |
| 设置页 | WiFi 重连 / WiFi 重置（重新配网）/ 版式 / 字体等 |

## SD 卡文件结构

```
/font/ 或根目录下的字体文件          .eefont / .bin（根目录读取）
/weread/config.json                WiFi 多账号 + cookie + 字体/版式选择
/weread/progress.json              各书本地阅读进度
/weread/state.json                 上次阅读位置
/weread/shelf_cache.json           书架缓存（离线模式用）
/weread/imgoff.json                各书插图开关
/weread/cache/<bookId>/            整本下载缓存（toc.json / detail.json / ch_<uid>.blk）
/weread/img/                       封面与插图缓存
```

## 项目结构

```
main/                  应用入口
components/
  eego_hal/            硬件抽象（墨水屏驱动 / 触控 / RTC / 电源 / SD / 按键）
  eego_ui/             画布 + 字体渲染 + 图片解码（LodePNG / CPFONT / EDCBook）
  weread/              微信读书客户端（配网 / 登录 / API / 解密 / 缓存 / 应用框架）
  third_party/miniz/   ZIP/zlib 解压
web_flasher/           网页一键刷机工具（index.html + esptool-js bundle）
```

## 常见问题

- **屏幕字发虚/发白**：确认 SD 卡字体正常加载；检查字体文件完整性
- **登录又过期**：wr_skey 是短效令牌，设备会自动续期；wr_rt 失效才需重扫
- **拉取正文失败**：失败页显示原因（网络/登录/风控），点按重试
- **WiFi 连不上**：支持多账号自动切换；全部失败进离线模式（书架看已下载的书），设置页手动重连/重置配网
- **只连得上 2.4GHz**：墨水屏设备仅支持 2.4G、WiFi6 以下的网络
- **翻页有残影**：按需全刷清残影，属墨水屏正常物理现象

## 开源许可

本项目采用 **GPL-3.0**，代码中含 GPL 兼容的第三方组件，按各许可证要求标注如下：

| 组件 | 用途 | 许可证 | 来源 |
|------|------|--------|------|
| [LodePNG](https://github.com/lvandeve/lodepng) | PNG 解码 | zlib | `components/eego_ui/lodepng.{h,cpp}`，Copyright (c) 2005-2026 Lode Vandevenne |
| [miniz](https://github.com/richgel999/miniz) | ZIP/zlib 解压 | Unlicense（公有领域） | `components/third_party/miniz/`，RAD Game Tools / Rich Geldreich |
| [QR-Code-generator](https://github.com/nayuki/QR-Code-generator) | 登录二维码 | MIT | `components/weread/qrcodegen.{h,cpp}`，Project Nayuki |
| [weread.koplugin](https://github.com/finlater/weread.koplugin) | 微信读书协议加密实现参考 | 版权归原作者（本项目按其兼容性采用 GPL-3.0） | `weread_crypto` 移植自 `lib/weread.lua` 与 `scripts/fetch_weread_epub.py` |
| [esptool-js](https://github.com/EspressifSystems/esptool-js) | 网页烧录（Web Serial） | Apache-2.0 | `web_flasher/bundle.js`，Espressif Systems |
| [pako](https://github.com/nodeca/pako) | 浏览器端解压（bundle 内嵌） | MIT AND Zlib | nodeca/pako 2.1.0 |
| [HarmonyOS Sans SC](https://developer.huawei.com/consumer/cn/design/resource/) | 内置字体 | 华为字体许可（免费商用） | `HarmonyOS_Sans_SC/`、`eego-a4-weread-fonts/` |
| ESP-IDF | 编译框架/运行时 | Apache-2.0 | 乐鑫官方框架，编译依赖 |

> 注：`boot_logo.h` 为微信读书官方图标缩放转灰度，仅供本设备个人使用；发布前请按需替换。
> 使用本项目前请确保你的微信读书账号遵守相关服务条款，本固件仅供个人学习交流。

## 免责声明

本固件非微信读书官方产品，与腾讯微信读书无任何关联。项目仅用于个人学习与研究，
使用过程中产生的账号风险（如风控、封禁）由使用者自行承担。若你喜欢微信读书，请支持正版订阅。

## License

[GPL-3.0](LICENSE) © 项目作者，保留所有权利。
