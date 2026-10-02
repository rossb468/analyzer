// Helpers shared by the ffi tests.
//
// The tests call the C entry points exactly as Swift would - through the
// declarations in include/analyzer.h, with raw pointers and fixed buffers - so
// these helpers are only about files and strings, never about the API.

#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"

namespace analyzer::ffi::test {

// A path in the temporary directory for a test to write to. The name carries the
// test's own name, so tests running in parallel never share a file.
inline std::filesystem::path scratch(std::string_view name) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::filesystem::temp_directory_path() /
           (std::string("analyzer-ffi-") + info->test_suite_name() + "-" + info->name() + "-" +
            std::string(name));
}

// The path as the NUL-terminated string the C API takes.
inline std::string c_path(const std::filesystem::path& path) {
    return path.string();
}

inline std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

inline void write_text(const std::filesystem::path& path, std::string_view text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << text;
}

// The message in a status, which must stay valid UTF-8 however it was cut.
inline std::string message_of(const AnalyzerStatus& status) {
    const std::string text(status.message);
    EXPECT_TRUE(is_valid_utf8(text)) << "message must stay valid UTF-8";
    return text;
}

// Whether `text` is nothing but repetitions of `unit`.
inline bool only_repeats_of(std::string_view text, std::string_view unit) {
    if (text.size() % unit.size() != 0) {
        return false;
    }
    for (std::size_t at = 0; at < text.size(); at += unit.size()) {
        if (text.substr(at, unit.size()) != unit) {
            return false;
        }
    }
    return true;
}

}  // namespace analyzer::ffi::test
