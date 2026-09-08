cmake_minimum_required(VERSION 3.20.0)

# Custom "zds,st7789v" devicetree binding (same as Lab 11/12/13) -- DTS_ROOT
# must be extended BEFORE find_package(Zephyr...) runs, or the
# devicetree compiler won't find it.
list(APPEND DTS_ROOT ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(lab14_heartbeat_watchdog_host)

target_sources(app PRIVATE src/main.c)
target_include_directories(app PRIVATE include)
