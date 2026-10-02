#include "cli/text.hpp"

#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <system_error>

#include "base/contract.hpp"

namespace analyzer::cli::text {

namespace {

// Room for the longest positional double (a 309-digit integer part), a sign,
// and the 60 fractional digits fixed() allows.
constexpr std::size_t kBufferSize = 512;

template <std::floating_point Float>
std::optional<std::string> special(Float value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }
    return std::nullopt;
}

bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

bool equals_ignoring_case(std::string_view text, std::string_view lower) noexcept {
    if (text.size() != lower.size()) {
        return false;
    }
    for (std::size_t i = 0; i < text.size(); ++i) {
        if (std::tolower(static_cast<unsigned char>(text[i])) != lower[i]) {
            return false;
        }
    }
    return true;
}

// Whether `text` is a float literal Rust would accept: [+-] then inf, infinity,
// nan, or digits with an optional fraction (either side may be empty, not both)
// and an optional exponent. strtod alone would also take hex floats, leading
// white space and a trailing garbage tail, none of which Rust does.
bool is_float_literal(std::string_view text) noexcept {
    if (!text.empty() && (text.front() == '+' || text.front() == '-')) {
        text.remove_prefix(1);
    }
    if (equals_ignoring_case(text, "inf") || equals_ignoring_case(text, "infinity") ||
        equals_ignoring_case(text, "nan")) {
        return true;
    }
    std::size_t i = 0;
    std::size_t digits = 0;
    while (i < text.size() && is_digit(text[i])) {
        ++i;
        ++digits;
    }
    if (i < text.size() && text[i] == '.') {
        ++i;
        while (i < text.size() && is_digit(text[i])) {
            ++i;
            ++digits;
        }
    }
    if (digits == 0) {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        std::size_t exponent = 0;
        while (i < text.size() && is_digit(text[i])) {
            ++i;
            ++exponent;
        }
        if (exponent == 0) {
            return false;
        }
    }
    return i == text.size();
}

}  // namespace

template <std::floating_point Float>
std::string display(Float value) {
    if (auto named = special(value)) {
        return *named;
    }
    char buffer[kBufferSize];
    const auto result =
        std::to_chars(buffer, buffer + kBufferSize, value, std::chars_format::fixed);
    ANALYZER_EXPECTS(result.ec == std::errc(), "a float always fits the formatting buffer");
    return std::string(buffer, result.ptr);
}

template <std::floating_point Float>
std::string debug(Float value) {
    std::string out = display(value);
    if (std::isfinite(value) && out.find('.') == std::string::npos) {
        out += ".0";
    }
    return out;
}

template <std::floating_point Float>
std::string fixed(Float value, int places) {
    ANALYZER_EXPECTS(places >= 0 && places <= 60, "at most 60 decimal places");
    if (auto named = special(value)) {
        return *named;
    }
    char buffer[kBufferSize];
    const auto result =
        std::to_chars(buffer, buffer + kBufferSize, value, std::chars_format::fixed, places);
    ANALYZER_EXPECTS(result.ec == std::errc(), "a float always fits the formatting buffer");
    return std::string(buffer, result.ptr);
}

template std::string display<float>(float);
template std::string display<double>(double);
template std::string debug<float>(float);
template std::string debug<double>(double);
template std::string fixed<float>(float, int);
template std::string fixed<double>(double, int);

std::string pad_left(std::string_view text, std::size_t width) {
    std::string out;
    if (text.size() < width) {
        out.assign(width - text.size(), ' ');
    }
    out.append(text);
    return out;
}

std::string pad_right(std::string_view text, std::size_t width) {
    std::string out(text);
    if (text.size() < width) {
        out.append(width - text.size(), ' ');
    }
    return out;
}

std::optional<double> parse_f64(std::string_view text) {
    if (!is_float_literal(text)) {
        return std::nullopt;
    }
    // The literal has been validated, so strtod reads all of it. The only
    // locale input is the decimal point, and the harness never calls setlocale.
    const std::string owned(text);
    return std::strtod(owned.c_str(), nullptr);
}

std::optional<float> parse_f32(std::string_view text) {
    if (!is_float_literal(text)) {
        return std::nullopt;
    }
    const std::string owned(text);
    return std::strtof(owned.c_str(), nullptr);
}

std::optional<std::size_t> parse_usize(std::string_view text) noexcept {
    if (!text.empty() && text.front() == '+') {
        text.remove_prefix(1);
    }
    if (text.empty() || !is_digit(text.front())) {
        return std::nullopt;
    }
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace analyzer::cli::text
