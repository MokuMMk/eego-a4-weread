# main 组件
idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES eego_hal eego_ui weread
)
