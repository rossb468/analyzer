#include "plot/axis.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>

#include "base/contract.hpp"
#include "base/numeric.hpp"

namespace analyzer::plot {

FrequencyAxis::FrequencyAxis(float min_hz, float max_hz, float width)
    : min_hz_(min_hz), max_hz_(max_hz), width_(width) {
    ANALYZER_EXPECTS(min_hz > 0.0f && max_hz > min_hz, "need 0 < min_hz < max_hz");
    ANALYZER_EXPECTS(width > 0.0f, "width must be positive");
    log_min_ = std::log10(min_hz);
    log_span_ = std::log10(max_hz) - log_min_;
}

FrequencyAxis FrequencyAxis::audible(float width) {
    return FrequencyAxis(20.0f, 20000.0f, width);
}

float FrequencyAxis::freq_to_x(float hz) const noexcept {
    if (hz <= 0.0f) {
        return -std::numeric_limits<float>::infinity();
    }
    return (std::log10(hz) - log_min_) / log_span_ * width_;
}

float FrequencyAxis::x_to_freq(float x) const noexcept {
    return std::pow(10.0f, log_min_ + (x / width_) * log_span_);
}

FrequencyAxis FrequencyAxis::with_width(float width) const {
    return FrequencyAxis(min_hz_, max_hz_, width);
}

FrequencyAxis FrequencyAxis::with_range(float min_hz, float max_hz) const {
    return FrequencyAxis(min_hz, max_hz, width_);
}

std::vector<Tick> FrequencyAxis::ticks() const {
    const int first = saturating_cast<int>(std::floor(std::log10(min_hz_)));
    const int last = saturating_cast<int>(std::ceil(std::log10(max_hz_)));

    std::vector<Tick> ticks;
    for (int decade = first; decade <= last; ++decade) {
        const float base = std::pow(10.0f, static_cast<float>(decade));
        for (int step = 1; step <= 9; ++step) {
            const float value = base * static_cast<float>(step);
            if (value < min_hz_ || value > max_hz_) {
                continue;
            }
            ticks.push_back({value, freq_to_x(value), step == 1});
        }
    }
    return ticks;
}

std::string format_frequency(float hz) {
    char buffer[32];
    if (hz >= 1000.0f) {
        const float k = hz / 1000.0f;
        if (std::abs(k - std::round(k)) < 0.05f) {
            std::snprintf(buffer, sizeof buffer, "%dk", saturating_cast<int>(std::round(k)));
        } else {
            std::snprintf(buffer, sizeof buffer, "%.1fk", static_cast<double>(k));
        }
    } else if (hz >= 10.0f) {
        std::snprintf(buffer, sizeof buffer, "%d", saturating_cast<int>(std::round(hz)));
    } else {
        std::snprintf(buffer, sizeof buffer, "%.1f", static_cast<double>(hz));
    }
    return buffer;
}

LevelAxis::LevelAxis(float min_db, float max_db, float height)
    : min_db_(min_db), max_db_(max_db), height_(height) {
    ANALYZER_EXPECTS(max_db > min_db, "need min_db < max_db");
    ANALYZER_EXPECTS(height > 0.0f, "height must be positive");
}

LevelAxis LevelAxis::full_scale(float height) {
    return LevelAxis(-120.0f, 0.0f, height);
}

float LevelAxis::db_to_y(float db) const noexcept {
    return (max_db_ - db) / (max_db_ - min_db_) * height_;
}

float LevelAxis::y_to_db(float y) const noexcept {
    return max_db_ - (y / height_) * (max_db_ - min_db_);
}

LevelAxis LevelAxis::with_height(float height) const {
    return LevelAxis(min_db_, max_db_, height);
}

LevelAxis LevelAxis::with_range(float min_db, float max_db) const {
    return LevelAxis(min_db, max_db, height_);
}

std::vector<Tick> LevelAxis::ticks(float step) const {
    ANALYZER_EXPECTS(step > 0.0f, "tick step must be positive");

    const int first = saturating_cast<int>(std::ceil(min_db_ / step));
    const int last = saturating_cast<int>(std::floor(max_db_ / step));
    // Guard against an absurd step producing millions of ticks. The subtraction
    // is in 64 bits: two saturated ints can be further apart than an int holds.
    const auto count = std::clamp<std::int64_t>(
        static_cast<std::int64_t>(last) - static_cast<std::int64_t>(first), 0, 1024);

    std::vector<Tick> ticks;
    for (std::int64_t i = 0; i <= count; ++i) {
        const float value = static_cast<float>(first + i) * step;
        if (value < min_db_ - 1e-3f || value > max_db_ + 1e-3f) {
            continue;
        }
        // Every second step reads as the significant one.
        const float halves = value / (step * 2.0f);
        ticks.push_back({value, db_to_y(value), std::abs(halves - std::trunc(halves)) < 1e-4f});
    }
    return ticks;
}

}  // namespace analyzer::plot
