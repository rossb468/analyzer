// Overlapped power spectrum estimation with averaging.
//
// A single FFT of anything noise-like is a very noisy estimate. Welch's method
// averages the power spectra of many overlapped frames, which trades time
// resolution for variance. Overlapping is close to free: the window is already
// attenuating the frame edges, so reusing samples recovers the information the
// taper threw away and yields more averages per second.
//
// Level conventions
//
// SpectrumAnalyzer::power() is mean-square power per bin, scaled so that
// summing every bin of an unwindowed frame gives the mean square of the input
// (see the Parseval test). A bin-centred sinusoid of amplitude A reads A^2 / 2
// in its peak bin, for every window, because each window's correction factor is
// derived from that same window's DC gain.
//
// SpectrumAnalyzer::write_db_fs() uses 0 dBFS = full-scale sine, the usual
// convention for audio analysers. A sine of amplitude 1.0 therefore reads 0.0
// dBFS, not +3.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsp/complex.hpp"
#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace analyzer::dsp {

// Floor applied by SpectrumAnalyzer::write_db_fs() so that empty bins produce a
// finite number instead of negative infinity.
inline constexpr float kSpectrumFloorDb = -200.0f;

// Fraction of each frame reused by the next one.
//
// More overlap means more averages per second and a smoother display, at
// proportionally more CPU. 75% is the usual default for real-time work.
enum class Overlap {
    // Frames do not overlap; hop equals the frame size.
    None,
    // 50% overlap.
    Half,
    // 75% overlap.
    ThreeQuarters,
    // 87.5% overlap.
    SevenEighths,
};

// Samples advanced between consecutive frames of `size` samples. Never zero,
// however small the frame.
std::size_t overlap_hop(Overlap overlap, std::size_t size) noexcept;

// The overlap as a fraction of the frame, for display.
float overlap_fraction(Overlap overlap) noexcept;

// How successive frames are combined.
//
// A small value type rather than a bare enum because two modes take a
// parameter. Build one with the named constructors:
//
//   Averaging::none()
//   Averaging::exponential(0.2f)
//   Averaging::linear(10)
struct Averaging {
    enum class Mode {
        // Each frame replaces the previous one. Noisy, but maximally
        // responsive.
        None,
        // Exponential moving average: avg = alpha * new + (1 - alpha) * avg.
        //
        // Smaller alpha is smoother and slower. Construct from a time constant
        // with exponential_over().
        Exponential,
        // Average the first `frames` frames, then hold.
        //
        // This is the "10 averages and stop" behaviour measurement tools use,
        // not a sliding window - a true sliding average would need to retain
        // every frame.
        Linear,
        // Average every frame since the last reset. Variance keeps falling, so
        // this is the mode for a static measurement where precision matters
        // most.
        Infinite,
        // Retain the maximum seen in each bin since the last reset.
        PeakHold,
    };

    Mode mode = Mode::None;
    // Exponential only: the smoothing coefficient, clamped to 0..1 when used.
    float alpha = 0.0f;
    // Linear only: how many frames to average before holding.
    std::uint32_t frames = 0;

    static constexpr Averaging none() { return {Mode::None}; }
    static constexpr Averaging exponential(float alpha) { return {Mode::Exponential, alpha}; }
    static constexpr Averaging linear(std::uint32_t frames) { return {Mode::Linear, 0.0f, frames}; }
    static constexpr Averaging infinite() { return {Mode::Infinite}; }
    static constexpr Averaging peak_hold() { return {Mode::PeakHold}; }

    // Exponential averaging with a time constant of `seconds`, given the rate
    // at which frames arrive.
    //
    // Returns none() for a non-positive time constant, since that is what a
    // zero-length average means.
    static Averaging exponential_over(float seconds, float frames_per_second) noexcept;

    friend constexpr bool operator==(const Averaging&, const Averaging&) = default;
};

// How to set up a SpectrumAnalyzer.
struct SpectrumConfig {
    // Sample rate of the incoming audio, in hertz.
    float sample_rate = 48'000.0f;
    // FFT size in samples. Must be even and at least two.
    //
    // This is the resolution/response tradeoff: bin spacing is
    // sample_rate / size, but the frame also spans size / sample_rate seconds
    // of time that get averaged together.
    std::size_t size = 4096;
    // Analysis taper.
    WindowKind window = WindowKind::hann();
    // Frame overlap.
    Overlap overlap = Overlap::ThreeQuarters;
    // Averaging mode.
    Averaging averaging = Averaging::none();
};

// Welch-style overlapped spectrum estimator.
//
// Feed it samples with push(); it emits a frame every hop and folds each into
// the running average. All buffers are sized at construction, so push() does not
// allocate.
//
// Owned by the analysis thread; not safe to share without external
// synchronisation.
class SpectrumAnalyzer {
public:
    // Build an analyzer. `config.size` must be even and at least two, and
    // `config.sample_rate` must be positive.
    explicit SpectrumAnalyzer(const SpectrumConfig& config);

    // Feed samples in, returning how many frames completed.
    //
    // Accepts any length; partial frames are retained until the next call, so
    // the result is identical whether audio arrives in one block or many.
    std::size_t push(std::span<const float> samples) noexcept;

    // Mean-square power per bin. See the module comment for the exact
    // convention.
    std::span<const float> power() const noexcept { return power_; }

    // Write the spectrum as dBFS, where 0 dBFS is a full-scale sine. `out` must
    // be exactly bins() long.
    void write_db_fs(std::span<float> out) const noexcept;

    // Discard the running average and the partial frame.
    void reset() noexcept;

    // Change averaging mode. Resets the average, since mixing modes within one
    // accumulation would produce a number that means nothing.
    void set_averaging(Averaging averaging) noexcept;

    // Frames folded into the current average.
    std::uint32_t frames() const noexcept { return frames_; }

    // Number of bins, size / 2 + 1.
    std::size_t bins() const noexcept { return power_.size(); }

    // FFT size in samples.
    std::size_t size() const noexcept { return frame_.size(); }

    // Samples between consecutive frames.
    std::size_t hop() const noexcept { return hop_; }

    // Frames produced per second of audio at the configured rate.
    float frame_rate() const noexcept { return sample_rate_ / static_cast<float>(hop_); }

    // Width of one bin in hertz.
    float bin_spacing_hz() const noexcept { return sample_rate_ / static_cast<float>(size()); }

    // Centre frequency of bin `k`.
    float bin_frequency(std::size_t k) const noexcept {
        return static_cast<float>(k) * bin_spacing_hz();
    }

    // Effective noise bandwidth of each bin in hertz. Divide power by this to
    // get power spectral density.
    float enbw_hz() const noexcept { return window_.enbw_bins() * bin_spacing_hz(); }

    // The window in use.
    const Window& window() const noexcept { return window_; }

private:
    void process_frame() noexcept;
    void accumulate() noexcept;

    float sample_rate_;
    RealFft fft_;
    Window window_;
    std::size_t hop_;
    Averaging averaging_;

    // Sliding frame, size() long. `filled_` samples are valid.
    std::vector<float> frame_;
    std::size_t filled_ = 0;

    std::vector<float> windowed_;
    std::vector<Complex32> spectrum_;
    // Power of the frame currently being processed.
    std::vector<float> frame_power_;
    // The running average exposed to callers.
    std::vector<float> power_;
    std::uint32_t frames_ = 0;

    // 1 / (size * coherent_gain), precomputed.
    float amplitude_scale_;
};

}  // namespace analyzer::dsp
