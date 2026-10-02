// Status values, strings that cross the boundary, and the small amount of file
// plumbing the entry points share.

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <system_error>

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

namespace {

// The bytes of a string as unsigned values, which is what UTF-8 is defined on.
std::uint8_t byte_at(std::string_view text, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(text[index]);
}

// One step of a UTF-8 decode: the bytes consumed, and whether they formed a
// whole valid sequence.
//
// The accepted ranges are those of the Unicode standard's table of well-formed
// byte sequences, which rules out overlong forms, surrogates and anything above
// U+10FFFF. An invalid sequence consumes the longest prefix of one that was
// valid so far, so a replacement character stands for exactly the bytes that
// were wrong, which is how Rust's lossy conversion counts them.
struct Step {
    std::size_t length;
    bool valid;
};

Step decode_step(std::string_view text, std::size_t at) noexcept {
    const std::uint8_t lead = byte_at(text, at);
    if (lead < 0x80) {
        return {1, true};
    }

    std::size_t continuations = 0;
    std::uint8_t first_low = 0x80;
    std::uint8_t first_high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        continuations = 1;
    } else if (lead == 0xE0) {
        continuations = 2;
        first_low = 0xA0;
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        continuations = 2;
    } else if (lead == 0xED) {
        continuations = 2;
        first_high = 0x9F;
    } else if (lead == 0xF0) {
        continuations = 3;
        first_low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        continuations = 3;
    } else if (lead == 0xF4) {
        continuations = 3;
        first_high = 0x8F;
    } else {
        return {1, false};
    }

    std::size_t consumed = 1;
    for (std::size_t k = 0; k < continuations; ++k) {
        if (at + consumed >= text.size()) {
            return {consumed, false};
        }
        const std::uint8_t next = byte_at(text, at + consumed);
        const std::uint8_t low = k == 0 ? first_low : std::uint8_t{0x80};
        const std::uint8_t high = k == 0 ? first_high : std::uint8_t{0xBF};
        if (next < low || next > high) {
            return {consumed, false};
        }
        ++consumed;
    }
    return {consumed, true};
}

}  // namespace

std::size_t utf8_floor(std::string_view text, std::size_t end) noexcept {
    end = std::min(end, text.size());
    // A continuation byte (10xxxxxx) means `end` is inside a character.
    while (end > 0 && end < text.size() && (byte_at(text, end) & 0xC0) == 0x80) {
        --end;
    }
    return end;
}

bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        const Step step = decode_step(text, at);
        if (!step.valid) {
            return false;
        }
        at += step.length;
    }
    return true;
}

std::string utf8_lossy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const Step step = decode_step(text, at);
        if (step.valid) {
            out.append(text.substr(at, step.length));
        } else {
            out.append("\xEF\xBF\xBD");
        }
        at += step.length;
    }
    return out;
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

std::string os_error_text(int error) {
    return std::error_code(error, std::generic_category()).message() + " (os error " +
           std::to_string(error) + ")";
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
