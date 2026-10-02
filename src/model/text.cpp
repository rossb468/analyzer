#include "model/text.hpp"

#include <array>
#include <cstdint>

namespace analyzer::model::detail {

namespace {

// U+FFFD REPLACEMENT CHARACTER in UTF-8.
constexpr std::string_view kReplacement = "\xEF\xBF\xBD";

bool is_white_space(std::uint32_t code_point) noexcept {
    switch (code_point) {
        case 0x0009:
        case 0x000A:
        case 0x000B:
        case 0x000C:
        case 0x000D:
        case 0x0020:
        case 0x0085:
        case 0x00A0:
        case 0x1680:
        case 0x2028:
        case 0x2029:
        case 0x202F:
        case 0x205F:
        case 0x3000: return true;
        default: return code_point >= 0x2000 && code_point <= 0x200A;
    }
}

bool is_continuation(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0) == 0x80;
}

// Decode the code point starting at `text[0]`, with its length in bytes, or
// {0, 0} if the bytes there are not well-formed UTF-8.
std::pair<std::uint32_t, std::size_t> decode(std::string_view text) noexcept {
    if (text.empty()) {
        return {0, 0};
    }
    const auto lead = static_cast<unsigned char>(text[0]);
    std::size_t length = 0;
    std::uint32_t value = 0;
    if (lead < 0x80) {
        return {lead, 1};
    }
    if (lead >= 0xC2 && lead <= 0xDF) {
        length = 2;
        value = lead & 0x1Fu;
    } else if (lead >= 0xE0 && lead <= 0xEF) {
        length = 3;
        value = lead & 0x0Fu;
    } else if (lead >= 0xF0 && lead <= 0xF4) {
        length = 4;
        value = lead & 0x07u;
    } else {
        return {0, 0};
    }
    if (text.size() < length) {
        return {0, 0};
    }
    for (std::size_t i = 1; i < length; ++i) {
        if (!is_continuation(text[i])) {
            return {0, 0};
        }
        value = (value << 6) | (static_cast<unsigned char>(text[i]) & 0x3Fu);
    }
    // An overlong form encodes a small value in too many bytes; refuse it.
    constexpr std::array<std::uint32_t, 5> kSmallest = {0, 0, 0x80, 0x800, 0x10000};
    if (value < kSmallest[length]) {
        return {0, 0};
    }
    return {value, length};
}

}  // namespace

std::string_view trim(std::string_view text) noexcept {
    while (!text.empty()) {
        const auto [code_point, length] = decode(text);
        if (length == 0 || !is_white_space(code_point)) {
            break;
        }
        text.remove_prefix(length);
    }
    while (!text.empty()) {
        // Step back to the start of the last code point: at most three
        // continuation bytes follow its lead.
        std::size_t start = text.size() - 1;
        for (int back = 0; back < 3 && start > 0 && is_continuation(text[start]); ++back) {
            --start;
        }
        const auto [code_point, length] = decode(text.substr(start));
        if (length != text.size() - start || !is_white_space(code_point)) {
            break;
        }
        text.remove_suffix(length);
    }
    return text;
}

std::vector<std::string_view> lines(std::string_view text) {
    std::vector<std::string_view> out;
    while (!text.empty()) {
        const std::size_t newline = text.find('\n');
        if (newline == std::string_view::npos) {
            // The final line has no terminator, so a "\r" at its end is content.
            out.push_back(text);
            break;
        }
        std::string_view line = text.substr(0, newline);
        if (line.ends_with('\r')) {
            line.remove_suffix(1);
        }
        out.push_back(line);
        text.remove_prefix(newline + 1);
    }
    return out;
}

std::optional<std::pair<std::string_view, std::string_view>> split_once(std::string_view text,
                                                                        char separator) noexcept {
    const std::size_t at = text.find(separator);
    if (at == std::string_view::npos) {
        return std::nullopt;
    }
    return std::pair{text.substr(0, at), text.substr(at + 1)};
}

std::string utf8_lossy(std::span<const std::byte> bytes) {
    std::string out;
    out.reserve(bytes.size());
    const auto at = [&](std::size_t index) { return static_cast<unsigned char>(bytes[index]); };
    const auto replace = [&out] { out += kReplacement; };

    std::size_t i = 0;
    while (i < bytes.size()) {
        const unsigned char lead = at(i);
        if (lead < 0x80) {
            out.push_back(static_cast<char>(lead));
            ++i;
            continue;
        }

        // How many continuation bytes the lead wants, and the range its first
        // one is restricted to so overlong forms, surrogates and values above
        // U+10FFFF are refused.
        std::size_t continuations = 0;
        unsigned char low = 0x80;
        unsigned char high = 0xBF;
        if (lead >= 0xC2 && lead <= 0xDF) {
            continuations = 1;
        } else if (lead >= 0xE0 && lead <= 0xEF) {
            continuations = 2;
            if (lead == 0xE0) {
                low = 0xA0;
            } else if (lead == 0xED) {
                high = 0x9F;
            }
        } else if (lead >= 0xF0 && lead <= 0xF4) {
            continuations = 3;
            if (lead == 0xF0) {
                low = 0x90;
            } else if (lead == 0xF4) {
                high = 0x8F;
            }
        } else {
            replace();
            ++i;
            continue;
        }

        // Copy the sequence if it is whole; otherwise one replacement stands
        // for the part that was valid, and decoding resumes at the byte that
        // broke it.
        std::size_t length = 1;
        bool whole = true;
        for (std::size_t k = 0; k < continuations; ++k) {
            const std::size_t index = i + 1 + k;
            const unsigned char want_low = k == 0 ? low : 0x80;
            const unsigned char want_high = k == 0 ? high : 0xBF;
            if (index >= bytes.size() || at(index) < want_low || at(index) > want_high) {
                whole = false;
                break;
            }
            ++length;
        }
        if (whole) {
            for (std::size_t k = 0; k < length; ++k) {
                out.push_back(static_cast<char>(at(i + k)));
            }
        } else {
            replace();
        }
        i += length;
    }
    return out;
}

std::string debug_quote(std::string_view text) {
    static constexpr std::string_view kHex = "0123456789abcdef";
    const auto escape_code = [](std::string& out, unsigned value) {
        out += "\\u{";
        std::string digits;
        do {
            digits.insert(digits.begin(), kHex[value & 0xF]);
            value >>= 4;
        } while (value != 0);
        out += digits;
        out += '}';
    };

    std::string out = "\"";
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        const auto byte = static_cast<unsigned char>(c);
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\0': out += "\\0"; break;
            default:
                if (byte < 0x20 || byte == 0x7F) {
                    escape_code(out, byte);
                } else if (byte == 0xC2 && i + 1 < text.size() &&
                           static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
                           static_cast<unsigned char>(text[i + 1]) <= 0x9F) {
                    // U+0080..U+009F, the C1 control codes.
                    escape_code(out, static_cast<unsigned char>(text[i + 1]));
                    ++i;
                } else {
                    out += c;
                }
        }
    }
    out += '"';
    return out;
}

}  // namespace analyzer::model::detail
