# Contract checks, numeric conversions and Rust-compatible number text: the
# small pieces every other module needs. A real library rather than headers
# only, because the number formatting and parsing are too big to inline.
add_library(analyzer_base STATIC number_text.cpp)
add_library(analyzer::base ALIAS analyzer_base)
target_include_directories(analyzer_base PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_compile_features(analyzer_base PUBLIC cxx_std_20)
analyzer_warnings(analyzer_base)
analyzer_hardening(analyzer_base)
