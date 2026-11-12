# F3: MAX30102 driver + raw sample streaming over USB.
cmake_minimum_required(VERSION 3.20.0)

# Always send console output over USB (same as passing -S cdc-acm-console)
set(SNIPPET cdc-acm-console CACHE STRING "")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(max30102_stream LANGUAGES C)

target_sources(app PRIVATE
	src/main.c
	src/max30102.c
)
