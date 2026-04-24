cmake_minimum_required(VERSION 3.16)

# ── Board selection ───────────────────────────────────────────────────────────
#
#  Usage:
#    idf.py -DBOARD=esp32c3_dog     set-target esp32c3 build
#    idf.py -DBOARD=esp32s3_bedroom set-target esp32s3 build
#
#  The BOARD variable is cached so subsequent `idf.py build` calls remember it.
#  To switch boards: idf.py -DBOARD=<new_board> set-target <chip> fullclean build

if(NOT DEFINED BOARD)
    message(FATAL_ERROR
        "\n\n  No BOARD specified!\n"
        "  Run:  idf.py -DBOARD=esp32c3_dog     set-target esp32c3  build\n"
        "   or:  idf.py -DBOARD=esp32s3_bedroom set-target esp32s3  build\n")
endif()

set(BOARD ${BOARD} CACHE STRING "Active board name" FORCE)

# Resolve board header path (relative to project root, used as compiler include)
set(BOARD_CONFIG_DIR "${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}")
if(NOT EXISTS "${BOARD_CONFIG_DIR}/board_config.h")
    message(FATAL_ERROR "Board '${BOARD}' not found — expected: ${BOARD_CONFIG_DIR}/board_config.h")
endif()

# Pass the include path as a quoted string macro so config.h can #include it
add_compile_definitions(
    BOARD_CONFIG_HEADER="${BOARD_CONFIG_DIR}/board_config.h"
)

message(STATUS "━━━ Board: ${BOARD}  (${BOARD_CONFIG_DIR}/board_config.h) ━━━")

# ── Override sdkconfig.defaults per chip if a per-chip file exists ───────────
#  e.g. sdkconfig.defaults.esp32c3 is loaded automatically by IDF when
#  IDF_TARGET matches. No extra CMake needed — IDF handles it natively.

if(DEFINED DEVICE_NUMBER)
    add_compile_definitions(DEVICE_NUMBER=${DEVICE_NUMBER})
    message(STATUS "━━━ Device Number: ${DEVICE_NUMBER} ━━━")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(mojDogv1)