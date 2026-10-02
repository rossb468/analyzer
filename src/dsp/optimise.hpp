// Fitting parametric filters to the gap between a measurement and a target.
//
// The algorithm is greedy with local refinement, which is what most room
// correction does and is chosen here for a reason that matters more than
// elegance: the result has to be explainable. Every filter it produces
// corresponds to a feature you can point at on the measurement, so a user who
// disagrees with one can delete it and keep the rest. A global fit that solved
// for all filters at once would score better on residual error and produce
// filters that individually mean nothing.
//
// Each round finds the largest remaining error, places a peaking filter on it,
// refines that filter's three parameters by coordinate descent, subtracts its
// response from the residual, and repeats. Because filters cascade, their
// decibel responses add, so subtracting is exact rather than an approximation.
//
// Boost is capped far harder than cut
//
// This is the part that separates a correction that works from one that makes
// things worse. A dip in a room measurement is usually a cancellation - two
// paths arriving out of phase - and no amount of electrical gain fills it in,
// because the cancellation scales with the signal. What boosting a null does
// achieve is consuming headroom and driving the woofer harder at exactly the
// frequency where it is doing the least good.
//
// So OptimiserConfig::max_boost_db defaults well below
// OptimiserConfig::max_cut_db. Cutting a peak is nearly free and nearly
// always right; boosting a null is the classic way to burn excursion.
//
// It corrects the modal region by default
//
// OptimiserConfig::to_hz defaults to 500 Hz. Above the room's transition
// frequency the response varies enormously with microphone position, so a
// filter fitted to one position is fitted to noise as far as any other position
// is concerned. Below it the modes are properties of the room and correcting
// them helps everywhere. The band is configurable because the same machinery is
// useful for correcting a loudspeaker measured close up, where the whole range
// is meaningful.

#pragma once

#include <span>
#include <vector>

#include "dsp/eq.hpp"
#include "dsp/target.hpp"

namespace analyzer::dsp {

// How the fit is constrained.
struct OptimiserConfig {
    // Most filters to produce. The fit stops early once nothing is left worth
    // correcting.
    std::size_t max_filters = 8;
    // Low end of the corrected band, in hertz.
    float from_hz = 20.0f;
    // High end of the corrected band, in hertz.
    float to_hz = 500.0f;
    // Largest boost any one filter may apply. See the note at the top of this
    // file: deliberately asymmetric with max_cut_db.
    float max_boost_db = 3.0f;
    // Largest cut any one filter may apply.
    float max_cut_db = 12.0f;
    // Narrowest filter allowed. Very high Q corrects a single measurement
    // point, which is a property of the microphone position rather than of
    // the room.
    float max_q = 8.0f;
    // Widest filter allowed.
    float min_q = 0.5f;
    // Errors smaller than this are left alone.
    float threshold_db = 0.5f;
    // Rate the filters are designed at.
    float sample_rate = 48'000.0f;

    friend bool operator==(const OptimiserConfig&, const OptimiserConfig&) = default;
};

// What a fit produced.
struct Optimisation {
    // The filters, in the order they were placed - largest error first.
    std::vector<FilterBand> bands;
    // Root-mean-square error across the corrected band before any filter.
    float initial_error_db = 0.0f;
    // The same after every filter. Reported rather than asserted: how much
    // improvement is achievable depends entirely on the measurement.
    float final_error_db = 0.0f;

    // Improvement in the RMS error, in decibels. Negative would mean the fit
    // made things worse, which is worth being able to see.
    float improvement_db() const noexcept { return initial_error_db - final_error_db; }

    friend bool operator==(const Optimisation&, const Optimisation&) = default;
};

// Fit filters to the gap between `measured_db` and `target`.
//
// `frequencies` and `measured_db` are read pairwise up to the shorter of the
// two. Frequencies are expected to be ascending and log-spaced - which is what
// the plot's own column frequencies are - because every point then carries
// roughly equal weight per octave. Linearly spaced input would work but would
// weight the top octave about as heavily as everything below it put together.
//
// A nonsense configuration (a non-positive band edge, a NaN Q) is repaired to
// the defaults rather than obeyed; input with nothing usable in the band
// returns an empty Optimisation.
Optimisation optimise(std::span<const float> frequencies, std::span<const float> measured_db,
                      const TargetCurve& target, const OptimiserConfig& config = {});

}  // namespace analyzer::dsp
