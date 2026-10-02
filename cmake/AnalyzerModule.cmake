# Shared build settings, and the one function every module uses to declare
# itself. Keeping both here means a warning flag or a sanitizer is switched on
# for the whole core in one place rather than drifting module by module.

# Warnings for code this project owns. Third-party code is compiled without
# them, since its warnings are not ours to fix.
#
# -Wdouble-promotion and -Wfloat-conversion matter more than usual here: DSP
# runs in single precision on purpose, and an unnoticed `0.5` literal silently
# promotes a whole expression to double and back again.
#
# -Wnull-dereference is deliberately absent. GCC computes it after inlining, so
# at -O2 it fires inside libstdc++'s own stream code on correct callers, and a
# warning that only appears in Release builds cannot be held at zero.
function(analyzer_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
        if(ANALYZER_WERROR)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
            -Wdouble-promotion -Wfloat-conversion -Wimplicit-fallthrough
            -Wcast-align)
        if(ANALYZER_WERROR)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Debug builds turn on the standard library's own bounds checks, so an
# out-of-range `operator[]` aborts in tests instead of reading garbage. Rust
# did this unconditionally; here it costs nothing in release.
function(analyzer_hardening target)
    target_compile_definitions(${target} PUBLIC
        $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
        $<$<CONFIG:Debug>:_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_DEBUG>)
endfunction()

# No fused multiply-add contraction, anywhere, including third-party code.
#
# Compilers may turn a * b + c into one FMA instruction, which rounds once
# instead of twice. Apple Clang does so by default on arm64; GCC on x86-64 does
# not. The results differ in the last bit, which a 24-bit WAV of generated noise
# or a byte-for-byte golden comparison then exposes - the same core must produce
# the same numbers on every platform it ships on. The speed cost is negligible
# at this core's duty cycles. MSVC does not contract under its default
# /fp:precise.
if(NOT MSVC)
    add_compile_options(-ffp-contract=off)
endif()

if(ANALYZER_SANITIZE STREQUAL "address")
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer
                        -fno-sanitize-recover=undefined)
    add_link_options(-fsanitize=address,undefined)
elseif(ANALYZER_SANITIZE STREQUAL "thread")
    add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
    add_link_options(-fsanitize=thread)
elseif(NOT ANALYZER_SANITIZE STREQUAL "")
    message(FATAL_ERROR "ANALYZER_SANITIZE must be empty, 'address' or 'thread'")
endif()

# Declare a module: a static library named analyzer_<name>, aliased as
# analyzer::<name>, built from every .cpp in the module's directory.
#
# Sources are globbed rather than listed. With one directory per module and one
# file per concept, the directory listing already is the source list, and a
# hand-maintained copy of it only adds a place to forget a file.
#
#   analyzer_module(dsp DEPS analyzer::base kissfft)
function(analyzer_module name)
    cmake_parse_arguments(ARG "" "" "DEPS;SOURCES" ${ARGN})
    if(NOT ARG_SOURCES)
        file(GLOB ARG_SOURCES CONFIGURE_DEPENDS
             ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp ${CMAKE_CURRENT_SOURCE_DIR}/*.mm)
    endif()
    set(target analyzer_${name})
    add_library(${target} STATIC ${ARG_SOURCES})
    add_library(analyzer::${name} ALIAS ${target})
    # Everything includes by module path - "dsp/window.hpp" - from src/.
    target_include_directories(${target} PUBLIC ${PROJECT_SOURCE_DIR}/src)
    target_link_libraries(${target} PUBLIC ${ARG_DEPS})
    target_compile_features(${target} PUBLIC cxx_std_20)
    analyzer_warnings(${target})
    analyzer_hardening(${target})
endfunction()

# Declare a module's tests: one GoogleTest executable built from every
# *_test.cpp in the calling directory.
function(analyzer_module_tests name)
    cmake_parse_arguments(ARG "" "" "DEPS" ${ARGN})
    file(GLOB sources CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp)
    if(NOT sources)
        return()
    endif()
    set(target analyzer_${name}_tests)
    add_executable(${target} ${sources})
    target_link_libraries(${target} PRIVATE
        analyzer::${name} analyzer::test_support ${ARG_DEPS} GTest::gtest_main)
    analyzer_warnings(${target})
    # Death tests check that a broken precondition aborts with the right
    # message. The threadsafe style re-executes the binary, which is slower
    # but immune to whatever threads a test left running.
    gtest_discover_tests(${target}
        PROPERTIES ENVIRONMENT "GTEST_DEATH_TEST_STYLE=threadsafe"
        DISCOVERY_TIMEOUT 60)
endfunction()
