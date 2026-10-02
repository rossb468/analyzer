# The C++ core of analyzer.
#
# One static library per module, linked in the same direction the dependency
# graph runs: dsp, cal and plot are leaves; model and engine sit on dsp; audio
# stands alone; ffi sits on everything and is the only thing the apps link.
# A module that needs something it does not link against fails to compile,
# which is how the graph stays a graph.
#
#   cmake -S . -B build && cmake --build build && ctest --test-dir build
#
# ./check.sh runs the full gate: format, build with warnings as errors, and the
# tests under AddressSanitizer and UndefinedBehaviorSanitizer.

cmake_minimum_required(VERSION 3.24)
project(analyzer VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(ANALYZER_BUILD_TESTS "Build the unit tests" ${PROJECT_IS_TOP_LEVEL})
option(ANALYZER_WERROR "Treat compiler warnings as errors" ${PROJECT_IS_TOP_LEVEL})
set(ANALYZER_SANITIZE "" CACHE STRING
    "Sanitizers to build with: empty, 'address' (ASan + UBSan) or 'thread' (TSan)")

include(cmake/AnalyzerModule.cmake)

add_subdirectory(third_party)
add_subdirectory(src)

if(ANALYZER_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
