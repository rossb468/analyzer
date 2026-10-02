#include "base/number_text.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <clocale>
#include <cstdlib>
#include <system_error>
#include <type_traits>
#include <utility>

#include "base/contract.hpp"

namespace analyzer::text {

namespace {

// NaN and the infinities print the same whatever the precision.
template <std::floating_point Float>
std::optional<std::string> non_finite(Float value) {
    if (std::isnan(value)) {
        return "NaN";
    }
    if (std::isinf(value)) {
        return value < 0 ? "-inf" : "inf";
    }
    return std::nullopt;
}

bool equals_ignoring_case(std::string_view text, std::string_view lower) noexcept {
    return std::ranges::equal(text, lower, [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}

std::size_t count_digits(std::string_view text, std::size_t from) noexcept {
    std::size_t end = from;
    while (end < text.size() && text[end] >= '0' && text[end] <= '9') {
        ++end;
    }
    return end - from;
}

// Whether `text` is a finite number in Rust's grammar, after the sign.
bool is_decimal_literal(std::string_view text) noexcept {
    std::size_t i = 0;
    const std::size_t integer_digits = count_digits(text, i);
    i += integer_digits;
    std::size_t fraction_digits = 0;
    if (i < text.size() && text[i] == '.') {
        ++i;
        fraction_digits = count_digits(text, i);
        i += fraction_digits;
    }
    if (integer_digits == 0 && fraction_digits == 0) {
        return false;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        ++i;
        if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
            ++i;
        }
        const std::size_t exponent_digits = count_digits(text, i);
        if (exponent_digits == 0) {
            return false;
        }
        i += exponent_digits;
    }
    return i == text.size();
}

template <std::floating_point Float>
std::optional<Float> parse_real(std::string_view text) {
    bool negative = false;
    std::string_view body = text;
    if (!body.empty() && (body.front() == '+' || body.front() == '-')) {
        negative = body.front() == '-';
        body.remove_prefix(1);
    }

    if (equals_ignoring_case(body, "inf") || equals_ignoring_case(body, "infinity")) {
        const Float infinity = std::numeric_limits<Float>::infinity();
        return negative ? -infinity : infinity;
    }
    if (equals_ignoring_case(body, "nan")) {
        return std::numeric_limits<Float>::quiet_NaN();
    }
    if (!is_decimal_literal(body)) {
        return std::nullopt;
    }

    // The literal is now known to hold only digits, '.', 'e' and a sign, so the
    // one thing the C locale could change is the decimal point.
    std::string buffer(text);
    const char* point = std::localeconv()->decimal_point;
    if (const std::size_t dot = buffer.find('.');
        dot != std::string::npos && point != nullptr && std::string_view(point) != ".") {
        buffer.replace(dot, 1, point);
    }
    if constexpr (std::is_same_v<Float, float>) {
        return std::strtof(buffer.c_str(), nullptr);
    } else {
        return std::strtod(buffer.c_str(), nullptr);
    }
}

}  // namespace

template <std::floating_point Float>
std::string shortest(Float value) {
    if (auto text = non_finite(value)) {
        return *std::move(text);
    }

    // The shortest digits come from the scientific form, which says them
    // without the zeros that positional notation needs. Laying them out
    // ourselves is what keeps a large value from printing as its exact binary
    // expansion, which is what a positional to_chars would pick for 1e23.
    std::array<char, 64> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::scientific);
    ANALYZER_EXPECTS(result.ec == std::errc{}, "scientific form must fit in 64 characters");

    std::string_view text(buffer.data(), result.ptr);
    std::string out;
    if (text.front() == '-') {
        out += '-';
        text.remove_prefix(1);
    }
    const std::size_t e = text.find('e');
    std::string digits;
    for (const char c : text.substr(0, e)) {
        if (c != '.') {
            digits += c;
        }
    }
    std::string_view exponent_text = text.substr(e + 1);
    if (exponent_text.front() == '+') {
        exponent_text.remove_prefix(1);
    }
    int exponent = 0;
    std::from_chars(exponent_text.data(), exponent_text.data() + exponent_text.size(), exponent);

    // value = 0.DIGITS * 10^point.
    const int point = exponent + 1;
    const auto count = static_cast<int>(digits.size());
    if (point <= 0) {
        out += "0.";
        out.append(static_cast<std::size_t>(-point), '0');
        out += digits;
    } else if (point >= count) {
        out += digits;
        out.append(static_cast<std::size_t>(point - count), '0');
    } else {
        out.append(digits, 0, static_cast<std::size_t>(point));
        out += '.';
        out.append(digits, static_cast<std::size_t>(point));
    }
    return out;
}

template <std::floating_point Float>
std::string fixed(Float value, int places) {
    ANALYZER_EXPECTS(places >= 0 && places <= 60, "places must be between 0 and 60");
    if (auto text = non_finite(value)) {
        return *std::move(text);
    }
    // The widest finite double is 309 digits before the point.
    std::array<char, 400> buffer{};
    const auto result = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value,
                                      std::chars_format::fixed, places);
    ANALYZER_EXPECTS(result.ec == std::errc{}, "fixed form must fit in 400 characters");
    return std::string(buffer.data(), result.ptr);
}

template std::string shortest<float>(float);
template std::string shortest<double>(double);
template std::string fixed<float>(float, int);
template std::string fixed<double>(double, int);

std::string signed_fixed(double value, int places) {
    std::string text = fixed(value, places);
    if (!std::isnan(value) && !text.starts_with('-')) {
        text.insert(text.begin(), '+');
    }
    return text;
}

std::string pad_left(std::string_view text, std::size_t width) {
    std::string out(text.size() < width ? width - text.size() : 0, ' ');
    out += text;
    return out;
}

std::string pad_right(std::string_view text, std::size_t width) {
    std::string out(text);
    if (out.size() < width) {
        out.append(width - out.size(), ' ');
    }
    return out;
}

std::optional<double> parse_f64(std::string_view text) {
    return parse_real<double>(text);
}

std::optional<float> parse_f32(std::string_view text) {
    return parse_real<float>(text);
}

namespace {

template <std::unsigned_integral Unsigned>
std::optional<Unsigned> parse_unsigned(std::string_view text) noexcept {
    if (text.starts_with('+')) {
        text.remove_prefix(1);
    }
    // from_chars would also take a minus sign; there is no such thing as a
    // negative unsigned value, and "-0" is not one in Rust either.
    if (text.empty() || text.front() < '0' || text.front() > '9') {
        return std::nullopt;
    }
    Unsigned value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size()) {
        return std::nullopt;
    }
    return value;
}

}  // namespace

std::optional<std::uint32_t> parse_u32(std::string_view text) noexcept {
    return parse_unsigned<std::uint32_t>(text);
}

std::optional<std::size_t> parse_usize(std::string_view text) noexcept {
    return parse_unsigned<std::size_t>(text);
}

std::optional<bool> parse_bool(std::string_view text) noexcept {
    if (text == "true") {
        return true;
    }
    if (text == "false") {
        return false;
    }
    return std::nullopt;
}

}  // namespace analyzer::text
