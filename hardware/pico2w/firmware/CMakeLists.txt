# VW diagnostic interface firmware for the Pico 2 W board (PCB v0.2.0).
#
#   export PICO_SDK_PATH=/path/to/pico-sdk      (2.1 or newer, for pico2_w)
#   cmake -B build -G "Unix Makefiles" && cmake --build build
#   -> build/vw_diag.uf2, copy to the Pico in BOOTSEL mode
cmake_minimum_required(VERSION 3.13)

set(PICO_BOARD pico2_w CACHE STRING "Board type")
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(vw_diag C CXX ASM)
set(CMAKE_C_STANDARD 11)
pico_sdk_init()

# can2040: software CAN in PIO. GPL-3.0, so firmware built with it is GPL-3.0 too.
include(FetchContent)
FetchContent_Declare(can2040
    GIT_REPOSITORY https://github.com/KevinOConnor/can2040.git
    GIT_TAG v1.7.0)
FetchContent_MakeAvailable(can2040)   # no CMake project inside; this only downloads it

add_executable(vw_diag
    src/main.c
    src/app.c
    src/can_port.c
    src/frame.c
    src/isotp.c
    src/proto.c
    src/uds.c
    ${can2040_SOURCE_DIR}/src/can2040.c)

target_include_directories(vw_diag PRIVATE ${can2040_SOURCE_DIR}/src)
target_compile_options(vw_diag PRIVATE -Wall -Wextra)
target_link_libraries(vw_diag pico_stdlib cmsis_core hardware_pio hardware_irq hardware_clocks)

# Console on the Pico's USB port, not on the UART pins.
pico_enable_stdio_usb(vw_diag 1)
pico_enable_stdio_uart(vw_diag 0)
pico_add_extra_outputs(vw_diag)
