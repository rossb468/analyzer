#include "dsp/target.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace analyzer::dsp {

namespace {

// How sharply the room shelf turns over.
//
// Two makes the transition a gentle first-order-like shelf on a log axis,
// reaching half the shelf gain at the transition frequency. Higher would be a
// harder knee than any real room boundary produces.
constexpr float kShelfOrder = 2.0f;

// A slope in decibels per octave, zero at the reference frequency.
float tilt_db(float db_per_octave, float hz) noexcept {
    return db_per_octave * std::log2(hz / kReferenceHz);
}

// Linear interpolation in decibels against log frequency.
//
// Outside the listed range the curve holds its end value rather than
// extrapolating. A calibration or target file that stops at 20 kHz says nothing
// about 22 kHz, and inventing a continued slope there would be fabricating data
// at exactly the frequencies where it is least reliable.
float interpolate(std::span<const std::pair<float, float>> points, float hz) noexcept {
    if (points.empty()) {
        return 0.0f;
    }
    if (points.size() == 1) {
        return points.front().second;
    }

    const auto first = points.front();
    const auto last = points.back();
    if (hz <= first.first) {
        return first.second;
    }
    if (hz >= last.first) {
        return last.second;
    }

    const auto upper = std::partition_point(points.begin(), points.end(),
                                            [hz](const auto& point) { return point.first <= hz; });
    const auto [low_hz, low_db] = *(upper - 1);
    const auto [high_hz, high_db] = *upper;
    if (high_hz <= low_hz) {
        return low_db;
    }

    const float t = std::log(hz / low_hz) / std::log(high_hz / low_hz);
    return low_db + t * (high_db - low_db);
}

}  // namespace

TargetShape TargetShape::custom(std::vector<std::pair<float, float>> points) {
    std::erase_if(points, [](const std::pair<float, float>& point) {
        return !(point.first > 0.0f && std::isfinite(point.first) && std::isfinite(point.second));
    });
    std::stable_sort(points.begin(), points.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });

    TargetShape s;
    s.kind = Kind::Custom;
    s.points = std::move(points);
    return s;
}

float TargetShape::db_at(float hz) const noexcept {
    if (!std::isfinite(hz) || hz <= 0.0f) {
        return 0.0f;
    }
    switch (kind) {
        case Kind::Flat: return 0.0f;
        case Kind::Tilt: return tilt_db(db_per_octave, hz);
        case Kind::Room: {
            const float shelf = transition_hz > 0.0f
                                    ? shelf_db / (1.0f + std::pow(hz / transition_hz, kShelfOrder))
                                    : 0.0f;
            return shelf + tilt_db(db_per_octave, hz);
        }
        case Kind::Custom: return interpolate(points, hz);
    }
    return 0.0f;
}

void TargetCurve::set_offset_db(float offset_db) noexcept {
    if (std::isfinite(offset_db)) {
        offset_db_ = offset_db;
    }
}

void TargetCurve::write_levels(std::span<const float> frequencies,
                               std::span<float> out) const noexcept {
    const std::size_t count = std::min(frequencies.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = db_at(frequencies[i]);
    }
}

void TargetCurve::align_to(std::span<const float> frequencies, std::span<const float> measured_db,
                           float from_hz, float to_hz) noexcept {
    double sum = 0.0;
    std::uint32_t count = 0;

    const std::size_t pairs = std::min(frequencies.size(), measured_db.size());
    for (std::size_t i = 0; i < pairs; ++i) {
        const float hz = frequencies[i];
        const float measured = measured_db[i];
        if (hz < from_hz || hz > to_hz || !std::isfinite(measured)) {
            continue;
        }
        sum += static_cast<double>(measured - shape_.db_at(hz));
        ++count;
    }

    if (count > 0) {
        const auto offset = static_cast<float>(sum / static_cast<double>(count));
        if (std::isfinite(offset)) {
            offset_db_ = offset;
        }
    }
}

TargetCurve TargetCurve::aligned_to(std::span<const float> frequencies,
                                    std::span<const float> measured_db, float from_hz,
                                    float to_hz) const {
    TargetCurve aligned = *this;
    aligned.align_to(frequencies, measured_db, from_hz, to_hz);
    return aligned;
}

void TargetCurve::write_error(std::span<const float> frequencies,
                              std::span<const float> measured_db,
                              std::span<float> out) const noexcept {
    const std::size_t count = std::min({frequencies.size(), measured_db.size(), out.size()});
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = measured_db[i] - db_at(frequencies[i]);
    }
}

}  // namespace analyzer::dsp
