# miniz 单文件 zip/zlib 库（MIT 许可）
# 用于 EPUB 整本 zip 解包
idf_component_register(SRCS "miniz.c" "miniz_zip.c" "miniz_tinfl.c" "miniz_tdef.c"
                       INCLUDE_DIRS ".")
