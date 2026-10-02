// Broadband level metering: peak, RMS and LEQ.
//
// Weighting has to be a filter here
//
// The calibration module has A and C weighting as per-bin curves, which is the
// right shape for correcting a spectrum. A time-domain meter cannot use them:
// it never forms a spectrum, so there are no bins to correct. The same curves
// are therefore implemented again as cascaded biquads.
//
// Two independent implementations of the same standard is usually a smell, but
// here it is an asset - the tests check the filter's measured response against
// IEC 61672 and against the frequency-domain version, so the two have to agree
// or something is wrong.
//
// Level convention
//
// 0 dBFS is a full-scale sine, matching the spectrum analyzer. A sine of
// amplitude 1.0 has an RMS of 0.7071, so the meter adds 3.01 dB to a raw RMS
// figure. Without that, meters and spectrum would disagree by 3 dB about the
// same signal, which is exactly the sort of discrepancy that costs an afternoon.

#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "dsp/biquad.hpp"
#include "dsp/window.hpp"

namespace analyzer::dsp {

// Floor for a silent input.
inline constexpr float kMeterFloorDb = -200.0f;

// Weighting applied before measuring.
enum class MeterWeighting {
    // Unweighted.
    Z,
    // A-weighting.
    A,
    // C-weighting.
    C,
};

// Human-readable name, for test failure messages and logs.
const char* to_string(MeterWeighting weighting) noexcept;

// How fast the RMS detector responds.
//
// The time constants are from IEC 61672. Fast and Slow are exponential; Impulse
// is deliberately asymmetric, catching transients quickly and releasing slowly
// so a brief peak stays readable.
//
// A small value type rather than a bare enum because one kind, Custom, takes a
// parameter. Build one with the named constructors:
//
//   Integration::fast()
//   Integration::custom(0.4f)
struct Integration {
    enum class Kind {
        // 125 ms. The usual choice for live work.
        Fast,
        // 1 s. Steadier, for noise levels.
        Slow,
        // 35 ms attack, 1.5 s decay.
        Impulse,
        // An arbitrary symmetric time constant; see seconds.
        Custom,
    };

    Kind kind = Kind::Fast;
    // Custom only: the time constant in seconds.
    float seconds = 0.0f;

    static constexpr Integration fast() { return {Kind::Fast}; }
    static constexpr Integration slow() { return {Kind::Slow}; }
    static constexpr Integration impulse() { return {Kind::Impulse}; }
    static constexpr Integration custom(float time_constant) {
        return {Kind::Custom, time_constant};
    }

    friend constexpr bool operator==(const Integration&, const Integration&) = default;
};

// Human-readable name, for test failure messages and logs.
const char* to_string(Integration::Kind kind) noexcept;

// Broadband level meter.
//
// Real-time safe after construction: push() allocates nothing. Not
// thread-safe: one instance belongs to one thread at a time.
class LevelMeter {
public:
    // Build a meter. `sample_rate` must be positive.
    LevelMeter(float sample_rate, MeterWeighting weighting, Integration integration);

    // Feed samples.
    void push(std::span<const float> samples) noexcept;

    // Time-weighted RMS in dBFS.
    float rms_db() const noexcept;

    // Highest sample magnitude since the last reset, in dBFS.
    //
    // This is a sample peak, not a true peak: inter-sample overshoots between
    // converter samples are invisible to it. For measurement work that is the
    // honest figure; true-peak metering needs oversampling and belongs with
    // loudness compliance rather than acoustics.
    float peak_db() const noexcept;

    // Equivalent continuous level: the total energy since reset, expressed as
    // the steady level that would carry the same energy.
    float leq_db() const noexcept;

    // Seconds of audio integrated since reset.
    float elapsed_seconds() const noexcept;

    // Clear all state, including the filter's history.
    void reset() noexcept;

    // Clear only the peak hold.
    void reset_peak() noexcept { peak_ = 0.0f; }

    // The weighting in force.
    MeterWeighting weighting() const noexcept { return weighting_; }

    // The integration in force.
    Integration integration() const noexcept { return integration_; }

    // Window recommended for taking a calibration reading with this meter.
    //
    // Flat-top, for the same reason the spectrum uses it: its main lobe is flat,
    // so the level is right regardless of where a calibrator's tone falls
    // between bins.
    static constexpr WindowKind calibration_window() { return WindowKind::flat_top(); }

private:
    // A or C weighting as a biquad cascade. Declared here only so the meter can
    // hold one by value, which keeps push() free of indirection; defined in
    // meter.cpp.
    class WeightingFilter {
    public:
        WeightingFilter() = default;
        WeightingFilter(MeterWeighting weighting, float sample_rate);

        float process(float x) noexcept;
        void reset() noexcept;

    private:
        std::vector<Biquad> sections_;
        float gain_ = 1.0f;
    };

    float sample_rate_;
    WeightingFilter filter_;
    MeterWeighting weighting_;
    Integration integration_;
    float attack_;
    float decay_;

    float mean_square_ = 0.0f;
    float peak_ = 0.0f;
    // double because LEQ integrates for minutes or hours and float would stop
    // accumulating once the sum dwarfs each new sample.
    double energy_ = 0.0;
    std::uint64_t samples_ = 0;
};

}  // namespace analyzer::dsp
