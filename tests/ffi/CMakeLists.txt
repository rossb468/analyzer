# The C++ tests call the entry points exactly as Swift would. Linked with the
# allocation trap, so the audio callback that the offline-backend tests run is
# held to the real-time rules: any allocation inside it aborts.
analyzer_module_tests(ffi DEPS analyzer::alloc_trap)

# ---------------------------------------------------------------------------
# The header is plain C and the apps import it from Swift. These build the same
# C source against include/analyzer.h under each C standard, with warnings as
# errors, and link the whole ABI.
# ---------------------------------------------------------------------------

function(analyzer_c_abi_test name standard)
    add_executable(${name} c_compile_test.c)
    set_target_properties(${name} PROPERTIES
        C_STANDARD ${standard}
        C_STANDARD_REQUIRED ON
        C_EXTENSIONS OFF
        # The library is C++, so its runtime has to be linked even though this
        # file is not.
        LINKER_LANGUAGE CXX)
    target_link_libraries(${name} PRIVATE analyzer::ffi)
    if(NOT MSVC)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_test(NAME ${name} COMMAND ${name})
endfunction()

analyzer_c_abi_test(analyzer_ffi_c11_test 11)
if(NOT MSVC AND "c_std_23" IN_LIST CMAKE_C_COMPILE_FEATURES)
    analyzer_c_abi_test(analyzer_ffi_c23_test 23)

    # Compilers older than C23 take the `typedef uint32_t X` branch of every
    # enum, so the build above never sees `enum X : uint32_t`, the branch Swift
    # imports. This compiles the same file against a copy of the header with
    # that branch forced on, which any compiler that accepts a fixed underlying
    # type in C can check. Compile only: the declarations are the same.
    file(READ ${PROJECT_SOURCE_DIR}/include/analyzer.h analyzer_header_text)
    string(REPLACE "__STDC_VERSION__ >= 202311L" "1" analyzer_header_text "${analyzer_header_text}")
    file(CONFIGURE OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/c23_enum_branch/analyzer.h
         CONTENT "${analyzer_header_text}" @ONLY NEWLINE_STYLE UNIX)
    add_library(analyzer_ffi_c23_enum_branch OBJECT c_compile_test.c)
    set_target_properties(analyzer_ffi_c23_enum_branch PROPERTIES
        C_STANDARD 23 C_STANDARD_REQUIRED ON C_EXTENSIONS OFF)
    target_include_directories(analyzer_ffi_c23_enum_branch PRIVATE
        ${CMAKE_CURRENT_BINARY_DIR}/c23_enum_branch)
    target_compile_options(analyzer_ffi_c23_enum_branch PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

# ---------------------------------------------------------------------------
# include/analyzer.h is hand-maintained; while the Rust crate that used to
# generate it still exists, it must stay declaration-for-declaration identical.
# Exit status 77 means there is nothing to compare against (no cargo, or the
# crate is gone), which ctest reports as skipped rather than passed.
# ---------------------------------------------------------------------------

find_package(Python3 COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME ffi_header_equivalence
             COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/header_test.py
                     --header ${PROJECT_SOURCE_DIR}/include/analyzer.h)
    set_tests_properties(ffi_header_equivalence PROPERTIES SKIP_RETURN_CODE 77)
endif()
