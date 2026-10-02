analyzer_module_tests(model)

# The golden tests read the files the Rust core wrote, and compare what this
# code writes against them. See tests/golden/README.md.
if(TARGET analyzer_model_tests)
    target_compile_definitions(analyzer_model_tests PRIVATE
        ANALYZER_GOLDEN_DIR="${PROJECT_SOURCE_DIR}/tests/golden/fixtures")
endif()
