# eego_ui 组件 - 画布 + 字体渲染 + 图片解码
# (PortraitCanvas / EDCBook .bin / CPFONT v4 .eefont / 字体管理器 / ImageRenderer)
idf_component_register(
    SRCS
        "eego_ui.cpp"
        "portrait_canvas.cpp"
        "edcbook_font.cpp"
        "cpfont_renderer.cpp"
        "font_manager.cpp"
        "image_renderer.cpp"
        "lodepng.cpp"
    INCLUDE_DIRS "include"
    # esp_http_client: HTTPS 图片下载
    # mbedtls: MD5(URL) 缓存键
    # esp_rom: ESP32-S3 ROM tjpgd (jd_prepare/jd_decomp) 头文件与符号
    # lodepng: 单文件 PNG 解码（MIT，源码同级目录，#include "lodepng.h"）
    REQUIRES eego_hal esp_driver_spi esp_http_client mbedtls esp_rom
)
