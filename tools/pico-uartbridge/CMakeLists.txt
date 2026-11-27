# Pico (RP2040) USB <-> UART bridge for the W022 debug console (576000 8N1).
#   export PICO_SDK_PATH=~/pico-sdk
#   cmake -S . -B build -DPICO_BOARD=pico && make -C build   -> build/uartbridge.uf2
cmake_minimum_required(VERSION 3.13)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)
project(uartbridge C CXX ASM)
pico_sdk_init()
add_executable(uartbridge main.c)
target_link_libraries(uartbridge pico_stdlib)
pico_enable_stdio_usb(uartbridge 1)
pico_enable_stdio_uart(uartbridge 0)
pico_add_extra_outputs(uartbridge)
