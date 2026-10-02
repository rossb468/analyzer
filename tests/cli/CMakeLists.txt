# One test executable for the module, as every module has, with two extras: it
# needs the built analyzer-cli to run (harness_test.cpp and golden_test.cpp
# drive the real executable as a subprocess), and the golden test needs to
# know where the Rust core's recorded outputs are.
#
# Linked with the allocation trap, as the engine and audio tests are, so that
# run() exercised in-process has the callback guard armed.

if(NOT TARGET analyzer-cli)
    return()
endif()

analyzer_module_tests(cli DEPS analyzer::alloc_trap)

if(TARGET analyzer_cli_tests)
    # The executable has to exist before the tests that run it.
    add_dependencies(analyzer_cli_tests analyzer-cli)
    target_compile_definitions(analyzer_cli_tests PRIVATE
        ANALYZER_CLI_PATH="$<TARGET_FILE:analyzer-cli>"
        ANALYZER_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/fixtures")
endif()
