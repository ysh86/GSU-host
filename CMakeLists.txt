cmake_minimum_required(VERSION 3.13)

# Needs PICO_SDK_PATH pointing at a pico-sdk checkout.
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(gsu_host C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

add_executable(gsu_host gsu_host.c)
target_link_libraries(gsu_host pico_stdlib hardware_gpio hardware_clocks)

pico_enable_stdio_usb(gsu_host 1)
pico_enable_stdio_uart(gsu_host 0)
pico_add_extra_outputs(gsu_host)
