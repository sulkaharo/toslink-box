# Firmware for the Pico TOSLINK board (hardware/README.md, "Board A").
#
# Out-of-tree dependencies, because neither belongs in this repo:
#   PICO_SDK_PATH       raspberrypi/pico-sdk
#   PICO_SPDIF_RX_PATH  elehobica/pico_spdif_rx  (BSD-2-Clause)
# pico/build.sh sets both and will clone them if they are missing.
#
# Target is RP2040 -- measured, not assumed: the attached board reports Board-ID RPI-RP2 in
# BOOTSEL, which is Pico 1. An RP2350 reports RPI-RP2350 and needs PICO_PLATFORM changed.

cmake_minimum_required(VERSION 3.13)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(pico_toslink C CXX ASM)
set(CMAKE_C_STANDARD 11)

pico_sdk_init()

# pico_spdif_rx's own CMakeLists links `pico_audio_32b` unconditionally, which would drag in
# pico_extras and the pico_audio_i2s_32b submodule -- an I2S OUTPUT stage this firmware has no use
# for. Verified before stubbing it rather than assumed: spdif_rx.c includes only spdif_rx.h, stdio,
# string, hardware/{pio,dma,irq,sync}, pico/stdlib and its own generated .pio.h headers, and
# contains no `audio_` symbol at all. The dependency is vestigial, left over from the samples.
#
# Without this stub the link fails at the very last step with
#     ld: cannot find -lpico_audio_32b
# after every object has compiled, which reads like a missing toolchain rather than a spurious
# dependency. An empty INTERFACE target satisfies it and pulls in nothing.
if (NOT TARGET pico_audio_32b)
    add_library(pico_audio_32b INTERFACE)
endif()

add_subdirectory($ENV{PICO_SPDIF_RX_PATH} pico_spdif_rx)

add_executable(toslink-rx toslink-rx.c)

target_link_libraries(toslink-rx
    pico_stdlib
    pico_spdif_rx
)

# Report over USB CDC. No UART: the board has no serial header wired and a UART stdio would
# silently discard every line.
pico_enable_stdio_usb(toslink-rx 1)
pico_enable_stdio_uart(toslink-rx 0)

pico_add_extra_outputs(toslink-rx)   # .uf2 for drag-and-drop flashing
