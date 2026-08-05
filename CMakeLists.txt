cmake_minimum_required(VERSION 3.16.0)
# 额外组件目录：第三方库（miniz 等）放在 components/third_party/ 下
set(EXTRA_COMPONENT_DIRS "components/third_party")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(papers3-weread-eegoa4)
