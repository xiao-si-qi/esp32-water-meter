# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 固件版本号：会写入 app 镜像描述，可通过 esp_app_get_description()->version 读取，
# 网页与 /api/status 都会显示它。发版时改这一行即可。
set(PROJECT_VER "1.15.0")

project(water_meter)
