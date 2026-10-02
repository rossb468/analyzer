# Header-only: the contract macros everything else includes.
add_library(analyzer_base INTERFACE)
add_library(analyzer::base ALIAS analyzer_base)
target_include_directories(analyzer_base INTERFACE ${PROJECT_SOURCE_DIR}/src)
target_compile_features(analyzer_base INTERFACE cxx_std_20)
