# SPDX-License-Identifier: GPL-3.0-or-later
# CAN Bus Monitor and Logger — Top-level CMake build file
#
# Copyright (C) 2026  Ahmad Rashed
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.

cmake_minimum_required(VERSION 3.18)

project(canbus_monitor
    VERSION 1.0.0
    DESCRIPTION "CAN Bus Monitor and Logger for Embedded Linux"
    LANGUAGES C
)

# ---------------------------------------------------------------------------
# Build-system policy
# ---------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Compiler hardening flags
# ---------------------------------------------------------------------------
add_compile_options(
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wformat=2
    -Wconversion
    -Werror=implicit-function-declaration
    $<$<CONFIG:Debug>:-g3 -fsanitize=address,undefined>
    $<$<CONFIG:Release>:-O2 -DNDEBUG>
)

add_link_options(
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>
)

# ---------------------------------------------------------------------------
# Source files
# ---------------------------------------------------------------------------
set(CANMON_SOURCES
    src/main.c
    src/can_socket.c
    src/frame_logger.c
    src/signal_handler.c
    src/ring_buffer.c
    src/cli_parser.c
    src/statistics.c
)

# ---------------------------------------------------------------------------
# Main executable
# ---------------------------------------------------------------------------
add_executable(canbus_monitor ${CANMON_SOURCES})

target_include_directories(canbus_monitor
    PRIVATE
        ${CMAKE_SOURCE_DIR}/include
)

# pthread is needed for log-flush thread; rt for clock_gettime on older glibc
find_package(Threads REQUIRED)
target_link_libraries(canbus_monitor PRIVATE Threads::Threads rt)

# ---------------------------------------------------------------------------
# Installation
# ---------------------------------------------------------------------------
include(GNUInstallDirs)
install(TARGETS canbus_monitor RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES docs/canbus_monitor.1
    DESTINATION ${CMAKE_INSTALL_MANDIR}/man1
    OPTIONAL
)

# ---------------------------------------------------------------------------
# Tests (Unity framework, header-only, GPL-compatible MIT licence)
# ---------------------------------------------------------------------------
enable_testing()
add_subdirectory(tests)

# ---------------------------------------------------------------------------
# Packaging (CPack)
# ---------------------------------------------------------------------------
set(CPACK_PACKAGE_NAME          "canbus-monitor")
set(CPACK_PACKAGE_VERSION       ${PROJECT_VERSION})
set(CPACK_PACKAGE_CONTACT       "ahmad.rashed93@gmail.com")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY ${PROJECT_DESCRIPTION})
set(CPACK_GENERATOR             "TGZ;DEB")
include(CPack)
