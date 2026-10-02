// Status values, strings that cross the boundary, and the small amount of file
// plumbing the entry points share.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <stdexcept>

#include "ffi/internal.hpp"

namespace analyzer::ffi {

AnalyzerStatus status_ok() noexcept {
    return AnalyzerStatus{};
}

AnalyzerStatus status_failure(std::string_view message) noexcept {
    AnalyzerStatus status{};
    status.code = 1;
    write_c_string(status.message, message);
    return status;
}

void set_status(AnalyzerStatus* out, const AnalyzerStatus& status) noexcept {
    if (out != nullptr) {
        *out = status;
    }
}

void write_c_string(std::span<char> dest, std::string_view text) noexcept {
    std::fill(dest.begin(), dest.end(), '\0');
    if (dest.empty()) {
        return;
    }
    const std::size_t end = utf8_floor(text, std::min(text.size(), dest.size() - 1));
    std::copy_n(text.begin(), end, dest.begin());
}

std::string read_c_string(std::span<const char> source) {
    const auto terminator = std::find(source.begin(), source.end(), '\0');
    return utf8_lossy(std::string_view(
        source.data(), static_cast<std::size_t>(std::distance(source.begin(), terminator))));
}

std::optional<std::string> checked_utf8(const char* text) {
    const std::string_view view(text);
    if (!is_valid_utf8(view)) {
        return std::nullopt;
    }
    return std::string(view);
}

namespace {

struct FileCloser {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};
using FilePtr = std::unique_ptr<std::FILE, FileCloser>;

}  // namespace

std::optional<std::string> write_file(const std::string& path, std::string_view bytes) {
    FilePtr file(std::fopen(path.c_str(), "wb"));
    if (!file) {
        return os_error_text(errno);
    }
    if (!bytes.empty() && std::fwrite(bytes.data(), 1, bytes.size(), file.get()) != bytes.size()) {
        return os_error_text(errno);
    }
    // Closing is what reports a write the disk refused after the fact.
    if (std::fclose(file.release()) != 0) {
        return os_error_text(errno);
    }
    return std::nullopt;
}

FileRead read_text_file(const std::string& path) {
    FileRead result;
    FilePtr file(std::fopen(path.c_str(), "rb"));
    if (!file) {
        const int error = errno;
        result.not_found = error == ENOENT;
        result.error = os_error_text(error);
        return result;
    }

    std::array<char, 8192> chunk;
    while (true) {
        const std::size_t count = std::fread(chunk.data(), 1, chunk.size(), file.get());
        result.text.append(chunk.data(), count);
        if (count < chunk.size()) {
            break;
        }
    }
    if (std::ferror(file.get()) != 0) {
        result.error = os_error_text(errno);
        result.text.clear();
        return result;
    }
    if (!is_valid_utf8(result.text)) {
        result.error = "stream did not contain valid UTF-8";
        result.text.clear();
    }
    return result;
}

float clamp_float(float value, float low, float high) {
    if (!(low <= high)) {
        throw std::invalid_argument("clamp bounds are inverted or NaN");
    }
    if (value < low) {
        return low;
    }
    if (value > high) {
        return high;
    }
    return value;
}

}  // namespace analyzer::ffi
