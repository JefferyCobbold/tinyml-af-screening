# Build script for the AF screening firmware.
# Zephyr apps are CMake projects: find_package(Zephyr) pulls in the whole RTOS,
# the board definition and every module enabled in prj.conf (e.g. TFLite Micro).

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(af_firmware LANGUAGES C CXX)

target_sources(app PRIVATE src/main.cpp)
target_include_directories(app PRIVATE src)
