// Helpers shared by the model tests: bytes and text in and out of files and
// strings.

#pragma once

#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model::test {

inline std::vector<std::byte> to_bytes(std::string_view text) {
    const auto bytes = std::as_bytes(std::span(text));
    return {bytes.begin(), bytes.end()};
}

inline std::string to_string(std::span<const std::byte> bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

// The whole file, or a test failure if it cannot be read.
inline std::vector<std::byte> read_bytes(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "cannot open " << path;
        return {};
    }
    const std::string text((std::istreambuf_iterator<char>(file)),
                           std::istreambuf_iterator<char>());
    return to_bytes(text);
}

inline std::string read_text(const std::filesystem::path& path) {
    return to_string(read_bytes(path));
}

// A path in the temporary directory for a test to write to. The name carries the
// test's own name, so tests running in parallel never share a file.
inline std::filesystem::path scratch(std::string_view name) {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    return std::filesystem::temp_directory_path() /
           (std::string("analyzer-model-") + info->test_suite_name() + "-" + info->name() + "-" +
            std::string(name));
}

}  // namespace analyzer::model::test
