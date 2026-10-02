#include "cal/curve.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <optional>
#include <utility>

#include "base/number_text.hpp"

namespace analyzer::cal {

namespace {

// ASCII whitespace only. The Rust used Unicode whitespace, but calibration
// files are ASCII or a single-byte codepage, and treating a stray 0xA0 byte as
// a separator would be a surprise rather than a feature.
constexpr std::string_view kWhitespace = " \t\n\v\f\r";

bool is_separator(char c) {
    return kWhitespace.find(c) != std::string_view::npos || c == ',';
}

std::string_view trim(std::string_view text) {
    const auto first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

// The next field of a line, or empty when the line is exhausted. Runs of
// separators count as one, so "20,  -1.5" has two fields.
std::string_view next_field(std::string_view& line) {
    const auto start = std::find_if_not(line.begin(), line.end(), is_separator);
    const auto stop = std::find_if(start, line.end(), is_separator);
    const std::string_view field(start, stop);
    line.remove_prefix(static_cast<std::size_t>(stop - line.begin()));
    return field;
}

// Parse a whole field as a float, or nothing if any of it is left over.
//
// Rust's `str::parse` rules, shared with every other text format in the core:
// a leading '+' is accepted, the C locale is ignored - so a host that has
// called setlocale() with a decimal-comma locale still reads "1000.5" as a
// vendor wrote it - and trailing text is refused. std::from_chars would do
// most of this, but its floating-point overloads are missing from the libc++
// that ships with the oldest macOS and iOS this core supports.
std::optional<float> parse_float(std::string_view field) {
    return text::parse_f32(field);
}

bool is_comment(std::string_view line) {
    return line.front() == '*' || line.front() == '#' || line.front() == ';' || line.front() == '"';
}

}  // namespace

ResponseCurve::ResponseCurve(std::vector<CurvePoint> points) : points_(std::move(points)) {
    std::erase_if(points_, [](const CurvePoint& p) {
        return !(p.hz > 0.0f && std::isfinite(p.hz) && std::isfinite(p.db));
    });
    // Stable, so that "first listed wins" below is well defined. Non-finite
    // frequencies are already gone, so plain `<` is a strict weak ordering.
    std::ranges::stable_sort(points_, {}, &CurvePoint::hz);
    const auto duplicates = std::ranges::unique(
        points_,
        [](float a, float b) { return std::abs(a - b) < std::numeric_limits<float>::epsilon(); },
        &CurvePoint::hz);
    points_.erase(duplicates.begin(), duplicates.end());
}

ResponseCurve ResponseCurve::parse(std::string_view text) {
    std::vector<CurvePoint> points;
    while (!text.empty()) {
        const auto newline = text.find('\n');
        std::string_view line = trim(text.substr(0, newline));
        text.remove_prefix(newline == std::string_view::npos ? text.size() : newline + 1);

        if (line.empty() || is_comment(line)) {
            continue;
        }
        const auto hz = parse_float(next_field(line));
        const auto db = parse_float(next_field(line));
        if (hz && db) {
            points.push_back({*hz, *db});
        }
    }
    return ResponseCurve(std::move(points));
}

float ResponseCurve::db_at(float hz) const noexcept {
    if (points_.empty() || !std::isfinite(hz) || hz <= 0.0f) {
        return 0.0f;
    }
    const CurvePoint& first = points_.front();
    const CurvePoint& last = points_.back();
    if (hz <= first.hz) {
        return first.db;
    }
    if (hz >= last.hz) {
        return last.db;
    }

    // Binary search for the bracketing pair. The two checks above guarantee
    // first.hz < hz < last.hz, so `upper` is neither 0 nor past the end.
    const auto upper = static_cast<std::size_t>(
        std::ranges::lower_bound(points_, hz, {}, &CurvePoint::hz) - points_.begin());
    const CurvePoint& low = points_[upper - 1];
    const CurvePoint& high = points_[upper];

    const float span = std::log10(high.hz) - std::log10(low.hz);
    if (span <= 0.0f) {
        return low.db;
    }
    const float t = (std::log10(hz) - std::log10(low.hz)) / span;
    return low.db + (high.db - low.db) * t;
}

std::string to_string(const ResponseCurve& curve) {
    if (curve.is_flat()) {
        return "flat";
    }
    const auto& points = curve.points();
    char buffer[96];
    std::snprintf(buffer, sizeof buffer, "%zu points, %.0f-%.0f Hz", points.size(),
                  static_cast<double>(points.front().hz), static_cast<double>(points.back().hz));
    return buffer;
}

}  // namespace analyzer::cal
