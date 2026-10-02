// Second-order sections.
//
// One biquad implementation for the whole module. The weighting filters in
// dsp/meter.hpp and the equaliser in dsp/eq.hpp both build on it, which
// matters more than it sounds: a filter that measures differently from how it
// sounds is the single most confusing bug an audio tool can have, and two
// implementations is how that happens.
//
// Design formulas
//
// The equaliser sections follow Robert Bristow-Johnson's Audio EQ Cookbook,
// which is the de facto standard for parametric EQ and what every other tool
// in this space implements. Matching it is deliberate: a filter exported to a
// miniDSP or a Behringer unit has to mean there what it meant here.

#pragma once

#include <span>

#include "dsp/complex.hpp"

namespace analyzer::dsp {

// A second-order section in transposed direct form II.
//
// Transposed form II is chosen over direct form I for its numerical
// behaviour with float coefficients: it keeps two state variables instead of
// four and puts the accumulation where rounding hurts least, which matters at
// low frequencies where the poles crowd the unit circle.
//
// Not thread-safe: the delay line is mutated by process(), so one instance
// belongs to one thread at a time.
class Biquad {
public:
    // Feed-forward coefficients, already normalised by a0.
    float b0 = 1.0f;
    float b1 = 0.0f;
    float b2 = 0.0f;
    // Feedback coefficients, already normalised by a0.
    float a1 = 0.0f;
    float a2 = 0.0f;

    // A pass-through.
    constexpr Biquad() = default;

    // Build from coefficients that are already normalised by a0. The parameters
    // are named for feed-forward (ff) and feedback (fb) rather than b and a,
    // which would shadow the members.
    constexpr Biquad(float ff0, float ff1, float ff2, float fb1, float fb2) noexcept
        : b0(ff0), b1(ff1), b2(ff2), a1(fb1), a2(fb2) {}

    // A section that changes nothing.
    static constexpr Biquad identity() noexcept { return {}; }

    // Build from raw coefficients, dividing through by a0.
    //
    // Returns identity() for a zero or non-finite a0, because a degenerate
    // design must not become a filter full of infinities that then poisons
    // everything downstream.
    static Biquad normalised(float ff0, float ff1, float ff2, float a0, float fb1,
                             float fb2) noexcept;

    // Filter one sample.
    float process(float x) noexcept {
        const float y = b0 * x + s1_;
        s1_ = b1 * x - a1 * y + s2_;
        s2_ = b2 * x - a2 * y;
        return y;
    }

    // Filter a block in place.
    void process_block(std::span<float> samples) noexcept {
        for (float& sample : samples) {
            sample = process(sample);
        }
    }

    // Clear the delay line without touching the coefficients.
    void reset() noexcept {
        s1_ = 0.0f;
        s2_ = 0.0f;
    }

    // Complex frequency response at `hz`.
    Complex32 response_at(float hz, float sample_rate) const noexcept;

    // Magnitude response at `hz`, as a linear ratio.
    float magnitude_at(float hz, float sample_rate) const noexcept;

    // Two sections are equal when coefficients and delay line both match, so a
    // section that has been running is not equal to a fresh one.
    friend constexpr bool operator==(const Biquad&, const Biquad&) = default;

    // ---------------------------------------------------------------- RBJ --

    // Peaking EQ: a bump or dip centred on `hz`, flat either side.
    //
    // The workhorse of a parametric equaliser, and the only shape that can
    // correct a room mode without disturbing its neighbours.
    static Biquad peaking(float hz, float q, float gain_db, float sample_rate) noexcept;

    // Low shelf: everything below `hz` lifted or cut by `gain_db`.
    static Biquad low_shelf(float hz, float q, float gain_db, float sample_rate) noexcept;

    // High shelf: everything above `hz` lifted or cut by `gain_db`.
    static Biquad high_shelf(float hz, float q, float gain_db, float sample_rate) noexcept;

    // Second-order lowpass, -3 dB at `hz` for `q` of 1/sqrt(2).
    static Biquad low_pass(float hz, float q, float sample_rate) noexcept;

    // Second-order highpass.
    static Biquad high_pass(float hz, float q, float sample_rate) noexcept;

    // Bandpass with unity gain at the centre.
    static Biquad band_pass(float hz, float q, float sample_rate) noexcept;

    // Notch: a null at `hz`, unity elsewhere.
    static Biquad notch(float hz, float q, float sample_rate) noexcept;

    // Allpass: flat magnitude, phase rotated through 360 degrees about `hz`.
    //
    // Useful for time alignment between drivers, where the goal is to move
    // phase without touching level.
    static Biquad all_pass(float hz, float q, float sample_rate) noexcept;

    // ------------------------------------------------- weighting building --

    // Two zeros at DC and a double pole at `omega`, bilinear-transformed.
    //
    // H(s) = s^2 / (s + w)^2, the block both weighting curves are made of.
    static Biquad double_pole_highpass(float omega, float sample_rate) noexcept;

    // Two real poles and two zeros at Nyquist, bilinear-transformed.
    //
    // H(s) = 1 / ((s + w1)(s + w2)).
    static Biquad two_pole_lowpass(float omega_a, float omega_b, float sample_rate) noexcept;

private:
    float s1_ = 0.0f;
    float s2_ = 0.0f;
};

// Pre-warp an analog pole so the bilinear transform lands it on the intended
// digital frequency.
//
// The bilinear transform compresses the frequency axis towards Nyquist. The
// 12194 Hz pole is halfway there at a 48 kHz sample rate, so without this it
// ends up well below where it belongs and C-weighting reads about 0.6 dB low at
// 8 kHz - inside the class 1 tolerance, but wrong for no good reason.
float prewarp(float omega, float sample_rate) noexcept;

}  // namespace analyzer::dsp
