# Copyright (C) 2025 Alif Semiconductor - All Rights Reserved.
# Use, distribution and modification of this code is permitted under the
# terms stated in the Alif Semiconductor Software License Agreement
#
# You should have received a copy of the Alif Semiconductor Software
# License Agreement with this file. If not, please write to:
# contact@alifsemi.com, or visit: https://alifsemi.com/license

cmake_minimum_required(VERSION 3.20.0)

# MLEK root (until it becomes a proper west module)
set(MLEK_ROOT ${CMAKE_CURRENT_LIST_DIR}/../../../../../modules/ml-embedded-evaluation-kit_DEV)

# Register MLEK as an extra Zephyr module
list(APPEND ZEPHYR_EXTRA_MODULES ${MLEK_ROOT}/source/lib/ports/zephyr)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(alif_asr)

# Silence "changed in GCC 7.1" warnings from C++ headers
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wno-psabi")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Wno-psabi")

# ---------------------------------------------------------------------------
# Build-time model and labels generation from MLEK resources
# ---------------------------------------------------------------------------
set(MLEK_SCRIPTS_DIR ${MLEK_ROOT}/scripts)
set(RESOURCES_PATH ${MLEK_ROOT}/resources_downloaded CACHE PATH
    "Path to MLEK resources")
setup_source_generator()

# Source generation output directories
set(SRC_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/src)
set(INC_GEN_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
file(MAKE_DIRECTORY ${SRC_GEN_DIR} ${INC_GEN_DIR})

# Conformer ExecuTorch model requires Ethos-U85 (E8 boards)
string(TOLOWER "ethos-u85-256" _NPU_CFG_ID)

set(ASR_MODEL_PATH
    ${RESOURCES_PATH}/asr/conformer_fp32_cln_wer_6_47_arm_delegate_${_NPU_CFG_ID}.pte
    CACHE FILEPATH "Path to the Conformer ASR ExecuTorch model (.pte)")

if (NOT EXISTS ${ASR_MODEL_PATH})
    message(FATAL_ERROR
        "Conformer ASR model not found at:\n  ${ASR_MODEL_PATH}\n"
        "Compile the model for Ethos-U85-256 and place it at the path above.\n"
        "See MLEK resources/asr/ for details.\n")
endif()

set(EXTRA_MODEL_CODE
    "/* Model parameters for alif_asr */"
    "extern const int   g_melSpecWindowSize = 512"
    "extern const int   g_melSpecHopSize    = 160"
    "extern const int   g_chunkSize         = 1500"
)

generate_model_code(
    MODEL_PATH  ${ASR_MODEL_PATH}
    DESTINATION ${SRC_GEN_DIR}
    EXPRESSIONS ${EXTRA_MODEL_CODE}
    NAMESPACE   "arm" "app" "asr"
)

# Labels file (SentencePiece pieces for Conformer)
set(ASR_LABELS_FILE ${MLEK_ROOT}/resources/asr/labels/librispeech_sp.pieces)

generate_labels_code(
    INPUT           ${ASR_LABELS_FILE}
    DESTINATION_SRC ${SRC_GEN_DIR}
    DESTINATION_HDR ${INC_GEN_DIR}
    OUTPUT_FILENAME Labels
)

file(GLOB ASR_GENERATED_SOURCES ${SRC_GEN_DIR}/*.cc)

# ---------------------------------------------------------------------------
# Local alif_ui library (Zephyr-compatible, with lv_port shim)
# ---------------------------------------------------------------------------
add_subdirectory(src/application/api/use_case/alif_ui)

# ---------------------------------------------------------------------------
# App include directories and compile definitions
# ---------------------------------------------------------------------------
target_include_directories(app PRIVATE
    src/application/main/include
    src/use_case/alif_asr/include
    ${INC_GEN_DIR}
)

target_compile_definitions(app PRIVATE ACTIVATION_BUF_SZ=CONFIG_ACTIVATION_BUF_SZ)

target_sources(app PRIVATE
    src/application/main/Main.cc
    src/use_case/alif_asr/src/MainLoop.cc
    src/use_case/alif_asr/src/UseCaseHandlerEt.cc
    src/use_case/alif_asr/src/AudioBackend.cpp
    ${ASR_GENERATED_SOURCES}
)

target_link_libraries(app PRIVATE
    mlek::asr_impl
    alif_ui_api
)
