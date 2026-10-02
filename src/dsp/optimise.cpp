#include "dsp/optimise.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>

#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

// One usable measurement point: where it is, and how far it still sits above
// the target.
struct Point {
    float hz;
    float error_db;
};

// Force the configuration into a shape the fit can use.
//
// std::fmax rather than std::max wherever the operand might be NaN: fmax
// returns the other argument, which is what a repair wants, while std::max
// would pass the NaN straight through.
OptimiserConfig validated(OptimiserConfig config) {
    const OptimiserConfig defaults;
    if (!std::isfinite(config.from_hz) || config.from_hz <= 0.0f) {
        config.from_hz = defaults.from_hz;
    }
    if (!std::isfinite(config.to_hz) || config.to_hz <= config.from_hz) {
        config.to_hz = std::fmax(config.from_hz * 2.0f, defaults.to_hz);
    }
    if (!std::isfinite(config.min_q) || config.min_q <= 0.0f) {
        config.min_q = defaults.min_q;
    }
    if (!std::isfinite(config.max_q) || config.max_q < config.min_q) {
        config.max_q = std::fmax(config.min_q, defaults.max_q);
    }
    config.max_boost_db = std::fmax(config.max_boost_db, 0.0f);
    config.max_cut_db = std::fmax(config.max_cut_db, 0.0f);
    if (!std::isfinite(config.threshold_db) || config.threshold_db < 0.0f) {
        config.threshold_db = defaults.threshold_db;
    }
    if (!std::isfinite(config.sample_rate) || config.sample_rate <= 0.0f) {
        config.sample_rate = defaults.sample_rate;
    }
    config.max_filters = std::min<std::size_t>(config.max_filters, 64);
    return config;
}

// Root-mean-square of the residual, accumulated in double so a long
// log-spaced sweep does not lose its small terms.
float rms(std::span<const Point> points) {
    if (points.empty()) {
        return 0.0f;
    }
    double sum = 0.0;
    for (const Point& p : points) {
        sum += static_cast<double>(p.error_db) * static_cast<double>(p.error_db);
    }
    return static_cast<float>(std::sqrt(sum / static_cast<double>(points.size())));
}

// The residual at one point if the section were applied to it.
float corrected_error(const Biquad& section, const Point& p, float sample_rate) noexcept {
    const float magnitude = section.magnitude_at(p.hz, sample_rate);
    return magnitude > 0.0f ? p.error_db + amplitude_to_db(magnitude) : p.error_db;
}

// Subtract a band's response from the residual.
//
// Filters cascade, so their decibel responses add and this is exact.
void apply(std::span<Point> points, const FilterBand& band, float sample_rate) {
    const Biquad section = band.design(sample_rate);
    for (Point& p : points) {
        p.error_db = corrected_error(section, p, sample_rate);
    }
}

// Residual RMS if this band were applied.
float score(const FilterBand& band, std::span<const Point> points, const OptimiserConfig& config) {
    const Biquad section = band.design(config.sample_rate);
    double sum = 0.0;
    for (const Point& p : points) {
        const double corrected =
            static_cast<double>(corrected_error(section, p, config.sample_rate));
        sum += corrected * corrected;
    }
    const auto count = static_cast<double>(std::max<std::size_t>(points.size(), 1));
    return static_cast<float>(std::sqrt(sum / count));
}

void try_candidate(const FilterBand& candidate, std::span<const Point> points,
                   const OptimiserConfig& config, FilterBand& best, float& best_score) {
    const float candidate_score = score(candidate, points, config);
    if (candidate_score < best_score) {
        best = candidate;
        best_score = candidate_score;
    }
}

// Coordinate descent over frequency, Q and gain.
//
// Deterministic by construction: fixed candidate multipliers, fixed round
// count, no randomness. Two runs on the same measurement must produce the same
// filters, or comparing two corrections becomes impossible.
FilterBand refine(FilterBand band, std::span<const Point> points, const OptimiserConfig& config) {
    constexpr int kRounds = 4;
    float step = 1.0f;

    for (int round = 0; round < kRounds; ++round) {
        FilterBand best = band;
        float best_score = score(band, points, config);

        const float factors[] = {
            1.0f - 0.10f * step,
            1.0f - 0.04f * step,
            1.0f + 0.04f * step,
            1.0f + 0.10f * step,
        };
        for (const float factor : factors) {
            FilterBand candidate = band;
            candidate.hz = std::clamp(band.hz * factor, config.from_hz, config.to_hz);
            try_candidate(candidate, points, config, best, best_score);

            candidate = band;
            candidate.q = std::clamp(band.q * factor, config.min_q, config.max_q);
            try_candidate(candidate, points, config, best, best_score);

            candidate = band;
            candidate.gain_db =
                std::clamp(band.gain_db * factor, -config.max_cut_db, config.max_boost_db);
            try_candidate(candidate, points, config, best, best_score);
        }

        band = best;
        step *= 0.5f;
    }

    return band;
}

// Guess a Q from how wide the feature is.
//
// Walks outwards from the peak until the error falls to half of it, and
// converts that width to a Q. A guess is enough because refinement moves it,
// but a good guess keeps refinement from having to travel far.
float estimate_q(std::span<const Point> points, float peak_hz, float peak_error,
                 const OptimiserConfig& config) {
    const float half = peak_error / 2.0f;
    const auto crossed = [&](float error) {
        return peak_error > 0.0f ? error < half : error > half;
    };

    const auto peak_it =
        std::find_if(points.begin(), points.end(), [&](const Point& p) { return p.hz >= peak_hz; });
    const std::size_t peak_index =
        peak_it == points.end() ? 0 : static_cast<std::size_t>(peak_it - points.begin());

    float low = points.empty() ? peak_hz : points.front().hz;
    for (std::size_t i = peak_index; i-- > 0;) {
        if (crossed(points[i].error_db)) {
            low = points[i].hz;
            break;
        }
    }

    float high = points.empty() ? peak_hz : points.back().hz;
    for (std::size_t i = peak_index + 1; i < points.size(); ++i) {
        if (crossed(points[i].error_db)) {
            high = points[i].hz;
            break;
        }
    }

    if (high <= low || low <= 0.0f) {
        return std::clamp(4.0f, config.min_q, config.max_q);
    }
    // Q is centre frequency over bandwidth, with the centre taken as the
    // geometric mean because the axis is logarithmic.
    const float bandwidth = high - low;
    const float centre = std::sqrt(low * high);
    return std::clamp(centre / bandwidth, config.min_q, config.max_q);
}

// Place and refine one filter on the largest remaining error.
//
// Returns nullopt once nothing exceeds the threshold, which is what stops the
// fit adding filters that correct half a decibel of nothing.
std::optional<FilterBand> place_filter(std::span<const Point> points,
                                       const OptimiserConfig& config) {
    if (points.empty()) {
        return std::nullopt;
    }

    // The largest error by magnitude. `>=` rather than `>` so that a tie goes
    // to the later point, which std::max_element would not do: the choice is
    // arbitrary, but picking the other would place a different first filter on
    // a symmetric measurement.
    const Point* peak = &points.front();
    for (const Point& p : points) {
        if (std::abs(p.error_db) >= std::abs(peak->error_db)) {
            peak = &p;
        }
    }
    const float peak_hz = peak->hz;
    const float peak_error = peak->error_db;

    if (std::abs(peak_error) < config.threshold_db) {
        return std::nullopt;
    }

    // A positive error is too much energy and wants a cut. The asymmetric caps
    // are the whole point - see the note at the top of the header.
    const float gain = peak_error > 0.0f ? std::fmax(-peak_error, -config.max_cut_db)
                                         : std::fmin(-peak_error, config.max_boost_db);

    const FilterBand start{FilterKind::Peaking, peak_hz, gain,
                           estimate_q(points, peak_hz, peak_error, config), true};

    const FilterBand refined = refine(start, points, config);
    // Refinement can shrink a filter to nothing if the feature was noise.
    if (std::abs(refined.gain_db) >= 0.05f) {
        return refined;
    }
    return std::nullopt;
}

}  // namespace

Optimisation optimise(std::span<const float> frequencies, std::span<const float> measured_db,
                      const TargetCurve& target, const OptimiserConfig& requested) {
    const OptimiserConfig config = validated(requested);

    // Residual starts as the error the correction has to remove, restricted to
    // points that are usable and inside the band.
    const std::size_t count = std::min(frequencies.size(), measured_db.size());
    std::vector<Point> points;
    points.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        const float hz = frequencies[i];
        const float measured = measured_db[i];
        if (hz >= config.from_hz && hz <= config.to_hz && std::isfinite(hz) &&
            std::isfinite(measured)) {
            points.push_back({hz, measured - target.db_at(hz)});
        }
    }

    if (points.empty()) {
        return {};
    }

    Optimisation result;
    result.initial_error_db = rms(points);

    for (std::size_t round = 0; round < config.max_filters; ++round) {
        const std::optional<FilterBand> band = place_filter(points, config);
        if (!band) {
            break;
        }
        apply(points, *band, config.sample_rate);
        result.bands.push_back(*band);
    }

    result.final_error_db = rms(points);
    return result;
}

}  // namespace analyzer::dsp
