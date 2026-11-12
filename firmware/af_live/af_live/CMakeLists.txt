# Live AF screening: MAX30102 -> signal processing -> quality gate -> int8 model.
cmake_minimum_required(VERSION 3.20.0)

# Always send console output over USB (same as passing -S cdc-acm-console)
set(SNIPPET cdc-acm-console CACHE STRING "")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(af_live LANGUAGES C CXX)

target_sources(app PRIVATE
	src/main.cpp
	src/sensor.c
	src/max30102.c
	src/af_session.c
	src/af_dsp.c
)
target_include_directories(app PRIVATE src)
