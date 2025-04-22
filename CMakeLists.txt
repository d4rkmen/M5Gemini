
# For more information about build system see
# https://docs.espressif.com/projects/esp-idf/en/latest/api-guides/build-system.html
# The following five lines of boilerplate have to be in your project's
# CMakeLists in this exact order for cmake to work correctly
cmake_minimum_required(VERSION 3.16)


add_definitions(-DHAVE_SETTINGS)
# add_definitions(-DHAVE_USB)
add_definitions(-DHAVE_SDCARD)
add_definitions(-DHAVE_MIC)
add_definitions(-DHAVE_SPEAKER)
add_definitions(-DHAVE_WIFI)
# add_definitions(-DHAVE_BATTERY)
file (STRINGS version.txt BUILD_NUMBER)
set(PROJECT_VER ${BUILD_NUMBER})
add_compile_options(-Wno-missing-field-initializers)
add_compile_definitions(BUILD_NUMBER="${BUILD_NUMBER}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

idf_build_set_property(MINIMAL_BUILD ON)
project(M5Gemini)
