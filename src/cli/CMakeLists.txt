# The headless harness: a library with everything except main(), so that it
# can be unit tested, and the analyzer-cli executable on top of it.
#
# Not built for iOS: there is no terminal to run it from.
#
# Sources are listed rather than globbed, for the same reason the audio module
# lists its own: which live-capture implementation is compiled depends on the
# platform, and the executable's main.cpp must stay out of the library. This is
# the CMake counterpart of the Rust `#[cfg(target_os = "macos")] mod
# live_coreaudio`.
#
#   macOS   live_coreaudio.cpp   captures through audio::default_backend()
#   other   live_unsupported.cpp says there is no backend, plainly

if(CMAKE_SYSTEM_NAME STREQUAL "iOS")
    return()
endif()

add_library(analyzer_cli_lib STATIC
    args.cpp
    bench.cpp
    live_display.cpp
    measure.cpp
    report.cpp
    run.cpp
    source.cpp
    text.cpp)
add_library(analyzer::cli ALIAS analyzer_cli_lib)

if(APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "iOS")
    target_sources(analyzer_cli_lib PRIVATE live_coreaudio.cpp)
else()
    target_sources(analyzer_cli_lib PRIVATE live_unsupported.cpp)
endif()

# Everything includes by module path - "cli/run.hpp" - from src/.
target_include_directories(analyzer_cli_lib PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(analyzer_cli_lib PUBLIC
    analyzer::base analyzer::dsp analyzer::model analyzer::engine analyzer::audio)
target_compile_features(analyzer_cli_lib PUBLIC cxx_std_20)
analyzer_warnings(analyzer_cli_lib)
analyzer_hardening(analyzer_cli_lib)

add_executable(analyzer-cli main.cpp)
target_link_libraries(analyzer-cli PRIVATE analyzer_cli_lib)
analyzer_warnings(analyzer-cli)

# The allocation trap is linked into debug builds only, mirroring the Rust
# harness's debug-only #[global_allocator]. It is what makes the rt_section
# around every audio callback mean something: with it, an allocation inside a
# callback aborts the process instead of quietly glitching. In release the
# trap would only add a branch to every allocation, so the plain allocator is
# used and rt_section merely counts, exactly as in the Rust.
target_link_libraries(analyzer-cli PRIVATE $<$<CONFIG:Debug>:analyzer::alloc_trap>)
