// Getting answers out of an impulse response.
//
// Gating, frequency response, and reverberation time. Together these are most
// of what an impulse response is *for*.
//
// Gating and the resolution it costs
//
// A room's impulse response contains the direct sound, then reflections, then a
// reverberant tail. Which of those you want depends on the question. Judging a
// loudspeaker means gating to the direct sound before the first wall reflection
// arrives; judging a room means keeping everything.
//
// Gating is not free, and the price is fixed by physics rather than by
// implementation: a gate of length T cannot resolve anything finer than 1/T in
// frequency. A 5 ms gate - typical for a domestic room, where the first
// reflection arrives around then - gives 200 Hz resolution, so the result says
// nothing meaningful below a few hundred hertz. That is why quasi-anechoic
// measurements are always spliced to a near-field or ground-plane measurement
// in the bass, and why GatedResponse::resolution_hz is reported rather than
// left for the user to work out.

#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <optional>
#include <span>
#include <vector>

#include "dsp/deconv.hpp"
#include "dsp/window.hpp"

namespace analyzer::dsp {

// Floor for decibel output.
inline constexpr float kIrFloorDb = -200.0f;

// A time window applied to an impulse response, in seconds relative to the
// direct arrival.
struct Gate {
    // Where the gate opens. Usually slightly negative, to keep the leading edge
    // of the direct arrival rather than slicing into it.
    float start_seconds = 0.0f;
    // Where the gate closes.
    float end_seconds = 0.0f;
    // Length of the taper at the closing edge.
    //
    // A hard cut is a rectangular window, and its spectral leakage smears the
    // response badly. Tapering the close costs a little time resolution and
    // removes the artefact.
    float fade_seconds = 0.0f;

    // A quasi-anechoic gate: the direct sound only, out to `end_seconds`.
    static constexpr Gate anechoic(float end_seconds) {
        return {.start_seconds = -0.001f,
                .end_seconds = end_seconds,
                .fade_seconds = std::min(end_seconds * 0.25f, 0.002f)};
    }

    // Gate length in seconds. Zero for a gate that closes before it opens.
    constexpr float length_seconds() const noexcept {
        const float length = end_seconds - start_seconds;
        return length > 0.0f ? length : 0.0f;
    }

    // The finest frequency resolution this gate permits.
    constexpr float resolution_hz() const noexcept {
        const float length = length_seconds();
        return length > 0.0f ? 1.0f / length : std::numeric_limits<float>::infinity();
    }

    friend constexpr bool operator==(const Gate&, const Gate&) = default;
};

// A frequency response derived from a gated impulse response.
struct GatedResponse {
    // Magnitude per bin in decibels.
    std::vector<float> magnitude_db;
    // Phase per bin in degrees.
    std::vector<float> phase_degrees;
    // Hertz between bins.
    float bin_spacing_hz = 0.0f;
    // Finest frequency the gate can resolve. Anything below this is an artefact
    // of the gate, not a property of the system.
    float resolution_hz = 0.0f;

    // Centre frequency of bin `index`.
    float bin_frequency(std::size_t index) const noexcept {
        return static_cast<float>(index) * bin_spacing_hz;
    }

    // Whether a frequency is above the gate's resolution limit and therefore
    // worth believing.
    bool is_trustworthy(float hz) const noexcept { return hz >= resolution_hz; }

    friend bool operator==(const GatedResponse&, const GatedResponse&) = default;
};

// Apply a gate to an impulse response, returning the windowed samples.
//
// The output is the gated region only, not the whole response zero-padded, so
// a caller can see how many samples actually survived.
std::vector<float> apply_gate(const ImpulseResponse& ir, const Gate& gate);

// Compute the frequency response of a gated impulse response.
//
// `fft_size` is zero-padded to, which interpolates the displayed curve without
// adding information - the real resolution is still set by the gate.
//
// Returns nullopt if the gate keeps nothing or `fft_size` is unusable (odd, or
// below 2).
std::optional<GatedResponse> gated_response(const ImpulseResponse& ir, const Gate& gate,
                                            std::size_t fft_size);

// Schroeder backward integration: the energy decay curve, in decibels.
//
// Integrating the squared response *backwards* from the end is the trick that
// makes reverberation time measurable from a single impulse. The raw squared
// response is far too noisy to fit a line to; the reverse cumulative integral
// of it is smooth, and its slope is the decay rate. Every RT60 measurement in
// the field is built on this.
//
// The curve is normalised to 0 dB at the start.
std::vector<float> schroeder_decay(const ImpulseResponse& ir);

// Reverberation time, estimated three ways.
//
// All three are extrapolations to a 60 dB decay, because a real room almost
// never has 60 dB of usable range above its noise floor. They are reported
// separately rather than averaged: when they disagree, that disagreement is
// the finding - a decay that is not a straight line means coupled spaces, a
// noise floor reached too early, or a measurement not worth trusting.
struct ReverbTime {
    // Early decay time, from the first 10 dB. Correlates best with what a
    // listener perceives as reverberance.
    std::optional<float> edt;
    // From the -5 to -25 dB span, extrapolated by three.
    std::optional<float> t20;
    // From the -5 to -35 dB span, extrapolated by two. Needs more range above
    // the noise floor than T20 but is less sensitive to early reflections.
    std::optional<float> t30;

    // The best available estimate, preferring the longest usable span.
    std::optional<float> best() const noexcept;

    // How far the estimates disagree, as a fraction of the largest.
    //
    // Above roughly 0.1 the decay is not a straight line and no single number
    // describes it. Nullopt with fewer than two estimates to compare.
    std::optional<float> spread() const noexcept;

    friend bool operator==(const ReverbTime&, const ReverbTime&) = default;
};

// Estimate reverberation time from a decay curve.
ReverbTime reverb_time(std::span<const float> decay_db, float sample_rate);

// Window kind recommended for transforming a gated response.
WindowKind recommended_gate_window();

// Build a window matching a gate's length, for callers that want the taper
// separately from the gating.
//
// Returns nullopt if the gate is shorter than one sample.
std::optional<Window> gate_window(const Gate& gate, float sample_rate);

}  // namespace analyzer::dsp
