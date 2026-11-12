# F4: full AF pipeline on the board, verified against Python golden data.
cmake_minimum_required(VERSION 3.20.0)

# Always send console output over USB (same as passing -S cdc-acm-console)
set(SNIPPET cdc-acm-console CACHE STRING "")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(af_pipeline LANGUAGES C CXX)

target_sources(app PRIVATE
	src/main.cpp
	src/af_dsp.c
)
target_include_directories(app PRIVATE src)
