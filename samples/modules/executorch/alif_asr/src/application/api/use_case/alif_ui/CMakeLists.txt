# Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
# SPDX-License-Identifier: Apache-2.0

#########################################################
#                   ALIF UI API library                 #
#########################################################
cmake_minimum_required(VERSION 3.21.0)

set(ALIF_UI_API_TARGET alif_ui_api)
project(${ALIF_UI_API_TARGET}
        DESCRIPTION "Alif UI API library (Zephyr port)"
        LANGUAGES   C CXX)

set(MLEK_ALIF_UI_SRC ${MLEK_ROOT}/source/lib/mlek/use_case/alif_ui/src)

add_library(${ALIF_UI_API_TARGET} STATIC
    src/lv_port.c
    ${MLEK_ALIF_UI_SRC}/ScreenLayout.cc
    ${MLEK_ALIF_UI_SRC}/lv_paint_utils.c
    ${MLEK_ALIF_UI_SRC}/Alif240.c
    ${MLEK_ALIF_UI_SRC}/Alif240_white.c)

# Our local include dir provides lv_port.h, ScreenLayout.hpp, lv_paint_utils.h
# and takes priority over the MLEK HAL include so Zephyr locking is used.
target_include_directories(${ALIF_UI_API_TARGET} PUBLIC include)

target_link_libraries(${ALIF_UI_API_TARGET} PUBLIC common_api PRIVATE zephyr_interface)

message(STATUS "*******************************************************")
message(STATUS "Library                                : " ${ALIF_UI_API_TARGET})
message(STATUS "CMAKE_SYSTEM_PROCESSOR                 : " ${CMAKE_SYSTEM_PROCESSOR})
message(STATUS "*******************************************************")
