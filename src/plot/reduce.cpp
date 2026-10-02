#include "plot/reduce.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/numeric.hpp"
#include "base/units.hpp"

namespace analyzer::plot {

namespace {

constexpr float kNegativeInfinity = -std::numeric_limits<float>::infinity();

// Linear interpolation between the values either side of `hz`, reading `fill`
// where there is nothing to read.
//
// Interpolating in decibels rather than power is deliberate here: this is a
// display value between two known points, and dB-linear is what looks right on
// a dB axis.
float interpolate(std::span<const float> values, float bin_spacing_hz, float hz, float fill) {
    if (values.empty()) {
        return fill;
    }
    const float exact = hz / bin_spacing_hz;
    if (exact <= 1.0f) {
        return values.size() > 1 ? values[1] : fill;
    }
    const auto lower = saturating_cast<std::size_t>(std::floor(exact));
    if (lower >= values.size() - 1) {
        return values.back();
    }
    const float a = values[lower];
    const float b = values[lower + 1];
    return a + (b - a) * (exact - static_cast<float>(lower));
}

float nearest(std::span<const float> values, float bin_spacing_hz, float hz, float fill) {
    const auto index =
        saturating_cast<std::size_t>(std::fmax(std::round(hz / bin_spacing_hz), 1.0f));
    return index < values.size() ? values[index] : fill;
}

// Visit every pixel column once and fill it from `dense` or `sparse`.
//
// Walk columns rather than bins. Bins can be visited more than once (sparse
// region) or many times per column (dense region), so the column is the thing
// that must be covered exactly once.
//
// `dense(span)` combines the bins that fall inside a column; `sparse(centre_hz)`
// is used when none does. Both are template parameters, not std::function, so
// they inline and the draw path never allocates.
template <typename Dense, typename Sparse>
void walk_columns(std::span<const float> values, float bin_spacing_hz, const FrequencyAxis& axis,
                  std::span<float> out, Dense dense, Sparse sparse) {
    const float column_width = axis.width() / static_cast<float>(out.size());

    for (std::size_t index = 0; index < out.size(); ++index) {
        const float x_left = static_cast<float>(index) * column_width;
        const float x_right = x_left + column_width;
        const float hz_left = axis.x_to_freq(x_left);
        const float hz_right = axis.x_to_freq(x_right);

        // Bin indices spanning this column, skipping DC.
        const auto first =
            saturating_cast<std::size_t>(std::fmax(std::ceil(hz_left / bin_spacing_hz), 1.0f));
        const auto last = std::min(
            saturating_cast<std::size_t>(std::floor(hz_right / bin_spacing_hz)), values.size() - 1);

        if (first <= last) {
            // Dense region: combine every bin in the column.
            out[index] = dense(values.subspan(first, last - first + 1));
        } else {
            // Sparse region: no bin falls inside, so interpolate between the two
            // that straddle the column centre.
            out[index] = sparse(axis.x_to_freq((x_left + x_right) * 0.5f));
        }
    }
}

}  // namespace

namespace detail {

float circular_mean_degrees(std::span<const float> degrees) noexcept {
    float x = 0.0f;
    float y = 0.0f;
    for (const float angle : degrees) {
        const float radians = angle * kRadiansPerDegree<float>;
        x += std::cos(radians);
        y += std::sin(radians);
    }
    if (x == 0.0f && y == 0.0f) {
        // Perfectly opposed angles cancel and have no mean direction. Zero is
        // as good an answer as any, and does not produce a NaN.
        return 0.0f;
    }
    return std::atan2(y, x) / kRadiansPerDegree<float>;
}

float mean_db(std::span<const float> levels) noexcept {
    if (levels.empty()) {
        return kNegativeInfinity;
    }
    float sum = 0.0f;
    for (const float db : levels) {
        sum += db_to_power(db);
    }
    const float mean = sum / static_cast<float>(levels.size());
    return mean > 0.0f ? power_to_db(mean) : kNegativeInfinity;
}

}  // namespace detail

Trace Trace::with_width(std::size_t width) {
    return Trace{std::vector<float>(width, kNegativeInfinity)};
}

void reduce(std::span<const float> bins, float bin_spacing_hz, const FrequencyAxis& axis,
            std::size_t columns, Reduction mode, Trace& out) {
    // assign() reuses the existing capacity, which is what keeps a recycled
    // trace allocation-free at a steady width, and overwrites every element, so
    // nothing from the previous frame survives a shrinking width.
    out.points.assign(columns, kNegativeInfinity);

    if (bins.size() < 2 || bin_spacing_hz <= 0.0f || columns == 0) {
        return;
    }

    auto dense = [mode](std::span<const float> column) {
        switch (mode) {
            case Reduction::Max: {
                float loudest = kNegativeInfinity;
                for (const float level : column) {
                    // fmax rather than std::max: a NaN bin is ignored, not
                    // allowed to wipe out the column.
                    loudest = std::fmax(loudest, level);
                }
                return loudest;
            }
            case Reduction::Mean: return detail::mean_db(column);
        }
        return kNegativeInfinity;
    };
    auto sparse = [&](float centre_hz) {
        return interpolate(bins, bin_spacing_hz, centre_hz, kNegativeInfinity);
    };
    walk_columns(bins, bin_spacing_hz, axis, out.points, dense, sparse);
}

void reduce_linear(std::span<const float> values, float bin_spacing_hz, const FrequencyAxis& axis,
                   std::size_t columns, LinearReduction mode, float fill, Trace& out) {
    out.points.assign(columns, fill);

    if (values.size() < 2 || bin_spacing_hz <= 0.0f || columns == 0) {
        return;
    }

    auto dense = [mode](std::span<const float> column) {
        switch (mode) {
            case LinearReduction::Min: {
                float lowest = std::numeric_limits<float>::infinity();
                for (const float value : column) {
                    lowest = std::fmin(lowest, value);
                }
                return lowest;
            }
            case LinearReduction::Mean: {
                float sum = 0.0f;
                for (const float value : column) {
                    sum += value;
                }
                return sum / static_cast<float>(column.size());
            }
            case LinearReduction::Circular: return detail::circular_mean_degrees(column);
        }
        return 0.0f;
    };
    auto sparse = [&](float centre_hz) {
        switch (mode) {
            // Interpolating an angle linearly wraps badly, so the nearest
            // bin is used instead. In a sparse region adjacent bins are
            // more than a pixel apart, and the error is invisible.
            case LinearReduction::Circular: return nearest(values, bin_spacing_hz, centre_hz, fill);
            case LinearReduction::Min:
            case LinearReduction::Mean: return interpolate(values, bin_spacing_hz, centre_hz, fill);
        }
        return fill;
    };
    walk_columns(values, bin_spacing_hz, axis, out.points, dense, sparse);
}

}  // namespace analyzer::plot
