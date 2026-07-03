# Copyright (c) 2025 Alif Semiconductor
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(usb_mass_storage)

target_sources(app PRIVATE src/main.c)

# Place USB host DMA buffers in SRAM0
zephyr_linker_sources(DATA_SECTIONS ${CMAKE_CURRENT_SOURCE_DIR}/uhc_dma.ld)
