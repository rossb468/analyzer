// Dual-FFT transfer function measurement.
//
// A spectrum shows what came out. It cannot show what the system *did*, because
// it does not know what went in. Measuring two channels at once - a reference
// of what was sent and a measurement of what was heard - and dividing one by
// the other cancels the source content entirely. That is what makes it possible
// to measure with pink noise, or with the actual programme material during a
// show, and get the same answer either way.
//
// The estimator
//
// With X the reference spectrum and Y the measurement:
//
//   Gxx = mean(|X|^2)          reference auto-power
//   Gyy = mean(|Y|^2)          measurement auto-power
//   Gxy = mean(Y * conj(X))    cross-spectrum
//
//   H1  = Gxy / Gxx            complex response
//   g^2 = |Gxy|^2 / (Gxx*Gyy)  coherence
//
// The averaging happens on the spectra, never on H itself. Averaging the ratio
// is the classic mistake here: it converges to something else, it does not
// suppress noise the same way, and it leaves coherence meaningless because
// coherence is defined in terms of those averaged products.
//
// Coherence needs more than one frame
//
// With a single frame |Gxy|^2 = Gxx*Gyy identically, so coherence is exactly 1
// no matter how noisy the measurement. It only becomes informative once several
// frames have been averaged and uncorrelated content has had a chance to cancel
// in the cross-spectrum while still summing in the auto-spectra.
// TransferFunction::frames() is exposed so a display can refuse to show
// coherence before it means anything.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsp/complex.hpp"
#include "dsp/fft.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/window.hpp"

namespace analyzer::dsp {

// Floor for magnitude output where the reference carries no energy.
inline constexpr float kMagnitudeFloorDb = -200.0f;

// How successive frames combine.
//
// Build one with infinite() or exponential(alpha).
struct TransferAveraging {
    enum class Mode {
        // Average everything since the last reset. Variance keeps falling, so
        // this is the mode for a static measurement.
        Infinite,
        // Exponential moving average, for a live display that must track
        // changes.
        Exponential,
    };

    Mode mode = Mode::Infinite;
    // Exponential only: smoothing coefficient in 0..1. Smaller is smoother and
    // slower.
    float alpha = 0.0f;

    static constexpr TransferAveraging infinite() { return {Mode::Infinite}; }
    static constexpr TransferAveraging exponential(float alpha) {
        return {Mode::Exponential, alpha};
    }

    friend constexpr bool operator==(const TransferAveraging&, const TransferAveraging&) = default;
};

// Setup for a TransferFunction.
struct TransferConfig {
    // Sample rate in hertz.
    float sample_rate = 48'000.0f;
    // FFT size. Must be even and at least two.
    std::size_t size = 4096;
    // Analysis window.
    WindowKind window = WindowKind::hann();
    // Frame overlap.
    Overlap overlap = Overlap::ThreeQuarters;
    // Averaging mode.
    TransferAveraging averaging = TransferAveraging::infinite();
};

// Two-channel transfer function estimator.
//
// Feed matched reference and measurement samples with push(). All buffers are
// sized at construction, so pushing does not allocate.
//
// Owned by the analysis thread; not safe to share without external
// synchronisation.
class TransferFunction {
public:
    // Build an estimator. `config.size` must be even and at least two, and
    // `config.sample_rate` must be positive.
    explicit TransferFunction(const TransferConfig& config);

    // Feed matched samples, returning how many frames completed.
    //
    // The two spans must be sample aligned: element n of each is the same
    // instant. That alignment is exactly why the capture ring carries
    // interleaved frames rather than one queue per channel - channels drifting
    // apart by a single sample would corrupt every phase reading here.
    //
    // The spans must be the same length. That is a programmer error, and
    // silently truncating to the shorter one would produce a plausible-looking
    // but wrong measurement.
    std::size_t push(std::span<const float> reference, std::span<const float> measurement) noexcept;

    // Magnitude of H1 in decibels. `out` must be exactly bins() long.
    void write_magnitude_db(std::span<float> out) const noexcept;

    // The complex response H1 itself.
    //
    // Magnitude and phase separately are what a display wants, but anything
    // that interpolates between frequencies needs the complex value: averaging
    // two phases across a +-180 degree wrap gives an answer pointing the wrong
    // way.
    //
    // `out` must be exactly bins() long.
    void write_response(std::span<Complex32> out) const noexcept;

    // Phase of H1 in degrees, wrapped to -180..180. `out` must be exactly
    // bins() long.
    void write_phase_degrees(std::span<float> out) const noexcept;

    // Coherence, 0..1.
    //
    // The fraction of the measurement linearly explained by the reference. Low
    // values mean noise, nonlinearity, or a timing mismatch, and mark the parts
    // of the response that should not be believed.
    //
    // Meaningless until several frames have been averaged - see the module
    // comment. `out` must be exactly bins() long.
    void write_coherence(std::span<float> out) const noexcept;

    // Frames folded into the current average.
    std::uint32_t frames() const noexcept { return frames_; }

    // Number of bins.
    std::size_t bins() const noexcept { return gxx_.size(); }

    // FFT size.
    std::size_t size() const noexcept { return reference_frame_.size(); }

    // Samples between frames.
    std::size_t hop() const noexcept { return hop_; }

    // Width of one bin in hertz.
    float bin_spacing_hz() const noexcept { return sample_rate_ / static_cast<float>(size()); }

    // Centre frequency of bin `index`.
    float bin_frequency(std::size_t index) const noexcept {
        return static_cast<float>(index) * bin_spacing_hz();
    }

    // Discard the average and any partial frame.
    void reset() noexcept;

private:
    void process_frame() noexcept;

    float sample_rate_;
    RealFft fft_;
    Window window_;
    std::size_t hop_;
    TransferAveraging averaging_;

    std::vector<float> reference_frame_;
    std::vector<float> measurement_frame_;
    std::size_t filled_ = 0;

    std::vector<float> windowed_;
    std::vector<Complex32> reference_spectrum_;
    std::vector<Complex32> measurement_spectrum_;

    std::vector<float> gxx_;
    std::vector<float> gyy_;
    std::vector<Complex32> gxy_;
    std::uint32_t frames_ = 0;
};

// Unwrap a wrapped phase curve in place, removing the +-360 degree jumps.
//
// Wrapped phase is what a display wants; unwrapped is what group delay and any
// slope measurement need, because a wrap looks like an infinite derivative.
void unwrap_phase_degrees(std::span<float> phase) noexcept;

}  // namespace analyzer::dsp
