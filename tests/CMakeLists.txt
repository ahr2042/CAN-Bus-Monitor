# SPDX-License-Identifier: GPL-3.0-or-later
# Tests sub-directory CMakeLists
#
# Uses the Unity test framework (ThrowTheSwitch/Unity) which is MIT licensed
# and therefore GPL-3.0-compatible.  We vendor the two required files to keep
# the build self-contained.

cmake_minimum_required(VERSION 3.18)

# ---------------------------------------------------------------------------
# Unity test framework (vendored — MIT licence, GPL-3.0 compatible)
# ---------------------------------------------------------------------------
set(UNITY_SRC ${CMAKE_CURRENT_SOURCE_DIR}/unity/unity.c)
set(UNITY_INC ${CMAKE_CURRENT_SOURCE_DIR}/unity)

# ---------------------------------------------------------------------------
# Helper macro to register a test executable
# ---------------------------------------------------------------------------
macro(add_canmon_test NAME)
    add_executable(${NAME} ${ARGN} ${UNITY_SRC})
    target_include_directories(${NAME}
        PRIVATE
            ${UNITY_INC}
            ${CMAKE_SOURCE_DIR}/include
    )
    target_link_libraries(${NAME} PRIVATE m)  # -lm for sqrt in statistics
    add_test(NAME ${NAME} COMMAND ${NAME})
endmacro()

# ---------------------------------------------------------------------------
# ring_buffer tests
# ---------------------------------------------------------------------------
add_canmon_test(test_ring_buffer
    test_ring_buffer.c
    ${CMAKE_SOURCE_DIR}/src/ring_buffer.c
)

# ---------------------------------------------------------------------------
# statistics tests  (needs -lm for sqrt)
# ---------------------------------------------------------------------------
add_canmon_test(test_statistics
    test_statistics.c
    ${CMAKE_SOURCE_DIR}/src/statistics.c
)

# ---------------------------------------------------------------------------
# cli_parser tests
# ---------------------------------------------------------------------------
add_canmon_test(test_cli_parser
    test_cli_parser.c
    ${CMAKE_SOURCE_DIR}/src/cli_parser.c
)

# ---------------------------------------------------------------------------
# frame_logger tests (uses tmp directory, no CAN socket needed)
# ---------------------------------------------------------------------------
add_canmon_test(test_frame_logger
    test_frame_logger.c
    ${CMAKE_SOURCE_DIR}/src/frame_logger.c
    ${CMAKE_SOURCE_DIR}/src/ring_buffer.c
)
target_link_libraries(test_frame_logger PRIVATE Threads::Threads)
