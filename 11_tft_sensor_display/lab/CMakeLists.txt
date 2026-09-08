cmake_minimum_required(VERSION 3.20.0)

# The custom "zds,st7789v" devicetree binding lives under this app's own
# dts/bindings/ -- DTS_ROOT must be extended BEFORE find_package(Zephyr...)
# runs, or the devicetree compiler won't find the binding and the build
# fails with "'zds,st7789v' compatible not found". Ported unchanged from
# jyounnim/zephyr_display-sr110's Lab 06 (06_TFT_ST7789V3), which
# hardware-verified this exact binding/driver approach on this board.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lab11_tft_sensor_display_host)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE include)
