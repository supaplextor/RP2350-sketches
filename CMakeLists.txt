cmake_minimum_required(VERSION 3.13)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2 CACHE STRING "Board type")
endif()

if(DEFINED ENV{PICO_SDK_PATH})
    set(PICO_SDK_PATH "$ENV{PICO_SDK_PATH}" CACHE PATH "Path to Pico SDK")
elseif(EXISTS "${CMAKE_CURRENT_LIST_DIR}/pico-sdk/external/pico_sdk_import.cmake")
    set(PICO_SDK_PATH "${CMAKE_CURRENT_LIST_DIR}/pico-sdk" CACHE PATH "Path to Pico SDK")
else()
    message(FATAL_ERROR
        "Pico SDK not found. Set PICO_SDK_PATH or clone pico-sdk into ./pico-sdk\n"
        "Example:\n"
        "  git clone https://github.com/raspberrypi/pico-sdk.git --depth=1\n"
        "  git -C pico-sdk submodule update --init\n"
        "  export PICO_SDK_PATH=$PWD/pico-sdk"
    )
endif()

include(${PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(rp2350_sketches C CXX ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

pico_sdk_init()

add_executable(serial_calc
    src/main.c
)

target_link_libraries(serial_calc
    pico_stdlib
)

pico_enable_stdio_usb(serial_calc 1)
pico_enable_stdio_uart(serial_calc 0)

pico_add_extra_outputs(serial_calc)
