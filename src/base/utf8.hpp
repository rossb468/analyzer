// UTF-8 checks and repair for text that arrives from outside.
//
// File contents, names from a device driver and strings handed over the C ABI
// are all bytes of unknown provenance. The core reads them as UTF-8 and has to
// decide what to do when they are not: refuse them (is_valid_utf8), or repair
// them with U+FFFD REPLACEMENT CHARACTER (utf8_lossy) so a stray byte in a
// measurement's name does not make the file unreadable.
//
// The accepted sequences are those of the Unicode standard's table of
// well-formed byte sequences, which rules out overlong forms, surrogates and
// anything above U+10FFFF. When a sequence is bad, one replacement character
// stands for the longest prefix of it that was valid so far - the same count
// Rust's `String::from_utf8_lossy` makes, which the model's files rely on.
//
// Header-only, so every module can use it without a link dependency.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace analyzer {

namespace detail {

// One step of a UTF-8 decode: the bytes consumed, and whether they formed a
// whole valid sequence.
struct Utf8Step {
    std::size_t length;
    bool valid;
};

inline std::uint8_t byte_at(std::string_view text, std::size_t index) noexcept {
    return static_cast<std::uint8_t>(text[index]);
}

// Decode the sequence starting at `text[at]`, which must exist.
inline Utf8Step decode_utf8_step(std::string_view text, std::size_t at) noexcept {
    const std::uint8_t lead = byte_at(text, at);
    if (lead < 0x80) {
        return {1, true};
    }

    // How many continuation bytes the lead wants, and the range the first of
    // them is held to. The later ones are always 0x80..0xBF.
    std::size_t continuations = 0;
    std::uint8_t first_low = 0x80;
    std::uint8_t first_high = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF) {
        continuations = 1;
    } else if (lead == 0xE0) {
        continuations = 2;
        first_low = 0xA0;  // below this would be an overlong form
    } else if ((lead >= 0xE1 && lead <= 0xEC) || lead == 0xEE || lead == 0xEF) {
        continuations = 2;
    } else if (lead == 0xED) {
        continuations = 2;
        first_high = 0x9F;  // above this would be a surrogate
    } else if (lead == 0xF0) {
        continuations = 3;
        first_low = 0x90;
    } else if (lead >= 0xF1 && lead <= 0xF3) {
        continuations = 3;
    } else if (lead == 0xF4) {
        continuations = 3;
        first_high = 0x8F;  // above this would pass U+10FFFF
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

}  // namespace detail

// Whether `text` is well-formed UTF-8.
inline bool is_valid_utf8(std::string_view text) noexcept {
    std::size_t at = 0;
    while (at < text.size()) {
        const detail::Utf8Step step = detail::decode_utf8_step(text, at);
        if (!step.valid) {
            return false;
        }
        at += step.length;
    }
    return true;
}

// `text` with each invalid sequence replaced by U+FFFD.
inline std::string utf8_lossy(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    std::size_t at = 0;
    while (at < text.size()) {
        const detail::Utf8Step step = detail::decode_utf8_step(text, at);
        if (step.valid) {
            out.append(text.substr(at, step.length));
        } else {
            out.append("\xEF\xBF\xBD");
        }
        at += step.length;
    }
    return out;
}

// Largest index at or below `end` that does not fall inside a UTF-8 sequence,
// for truncating text to a fixed buffer without cutting a character in half.
inline std::size_t utf8_floor(std::string_view text, std::size_t end) noexcept {
    end = std::min(end, text.size());
    // A continuation byte (10xxxxxx) means `end` is inside a character.
    while (end > 0 && end < text.size() && (detail::byte_at(text, end) & 0xC0) == 0x80) {
        --end;
    }
    return end;
}

}  // namespace analyzer
