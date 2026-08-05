# weread 组件 - 微信读书协议客户端 + 存储 + 应用逻辑
# (weread_client / weread_api / weread_crypto / weread_login / storage / config / UI 屏幕)
idf_component_register(
    SRCS
        "weread_app.cpp"
        "config.cpp"
        "storage.cpp"
        "wifi_manager.cpp"
        "provision_portal.cpp"
        "qrcodegen.cpp"
        "weread_crypto.cpp"
        "weread_client.cpp"
        "weread_api.cpp"
        "weread_login.cpp"
        "sleep_manager.cpp"
        "serial_debug.cpp"
    INCLUDE_DIRS "include"
    REQUIRES eego_hal eego_ui json sdmmc esp_wifi esp_event esp_netif esp_http_server esp_http_client mbedtls
    PRIV_REQUIRES esp_timer nvs_flash esp_driver_uart esp_hw_support app_update miniz
)

# qrcodegen 是纯 C++ 第三方库（Nayuki QR Code generator），使用 std::stdexcept 异常。
# ESP-IDF 默认 -fno-exceptions，单独为该文件启用异常支持。
set_source_files_properties(qrcodegen.cpp PROPERTIES COMPILE_FLAGS "-fexceptions")

# weread_app.cpp 使用 try-catch 捕获 QR 码生成异常，同样需要启用异常支持。
set_source_files_properties(weread_app.cpp PROPERTIES COMPILE_FLAGS "-fexceptions")

# 构建时间戳作为版本号（格式：YYYY-MM-DD HH:MM）
string(TIMESTAMP BUILD_VERSION "%Y-%m-%d %H:%M")
target_compile_definitions(${COMPONENT_LIB} PRIVATE FW_VERSION="${BUILD_VERSION}")
