// Analysis windows and their correction factors.
//
// The FFT treats its input frame as one period of an infinitely repeating
// signal. Unless the frame happens to contain a whole number of cycles, the
// wrap-around point is a discontinuity, and a discontinuity is broadband - a
// single clean tone smears across the whole spectrum. Tapering the frame to
// zero at both ends removes the discontinuity, at the cost of attenuating the
// signal and widening the response of each bin.
//
// Every window therefore carries two correction factors, and using the wrong
// one skews every reading by a constant that is easy to miss for months:
//
// - coherent_gain() - the mean of the window. A sinusoid's amplitude is scaled
//   by this, so recovering absolute level needs amplitude_correction().
// - enbw_bins() - equivalent noise bandwidth, in bins. Broadband signals spread
//   over more than one bin's nominal width, so power spectral density has to be
//   divided by this.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

namespace analyzer::dsp {

// Which taper to apply to an analysis frame.
//
// A small value type rather than a bare enum because one shape, Tukey, takes a
// parameter. Build one with the named constructors:
//
//   WindowKind::hann()
//   WindowKind::tukey(0.25f)
struct WindowKind {
    enum class Shape {
        // No taper. Correct only when the frame contains whole cycles, which in
        // practice means synthetic test signals.
        Rectangular,
        // General-purpose default: a reasonable balance of resolution and
        // leakage.
        Hann,
        // Low leakage, wider main lobe. For resolving small signals near large
        // ones.
        BlackmanHarris,
        // Flat main lobe for accurate amplitude. For calibration.
        FlatTop,
        // Tapered cosine; see tukey_alpha.
        Tukey,
    };

    Shape shape = Shape::Hann;
    // Tukey only: the fraction of the frame that is tapered. 0 is
    // rectangular, 1 is Hann. Values outside 0..1 are clamped.
    float tukey_alpha = 0.0f;

    static constexpr WindowKind rectangular() { return {Shape::Rectangular}; }
    static constexpr WindowKind hann() { return {Shape::Hann}; }
    static constexpr WindowKind blackman_harris() { return {Shape::BlackmanHarris}; }
    static constexpr WindowKind flat_top() { return {Shape::FlatTop}; }
    static constexpr WindowKind tukey(float alpha) { return {Shape::Tukey, alpha}; }

    friend constexpr bool operator==(const WindowKind&, const WindowKind&) = default;
};

// Human-readable name, for test failure messages and logs.
const char* to_string(WindowKind::Shape shape) noexcept;

// A precomputed analysis window with its correction factors.
//
// Built once when an analysis is configured; applying it afterwards does not
// allocate, so it is safe on the analysis thread.
class Window {
public:
    // Build a window of `size` samples. `size` must be non-zero.
    Window(WindowKind kind, std::size_t size);

    WindowKind kind() const noexcept { return kind_; }

    // Frame length in samples.
    std::size_t size() const noexcept { return samples_.size(); }

    // The window coefficients.
    std::span<const float> samples() const noexcept { return samples_; }

    // Mean of the window. A sinusoid passed through it comes out scaled by this.
    float coherent_gain() const noexcept { return coherent_gain_; }

    // Reciprocal of coherent_gain() - multiply a measured sinusoid amplitude by
    // this to recover its true level.
    float amplitude_correction() const noexcept;

    // Equivalent noise bandwidth in bins: how many bins' worth of noise each
    // bin actually collects. Divide power spectral density by this.
    float enbw_bins() const noexcept { return enbw_bins_; }

    // Apply the window in place. `frame` must be exactly size() samples.
    void apply(std::span<float> frame) const noexcept;

    // Apply the window into `output`, leaving `input` untouched. Both must be
    // exactly size() samples.
    void apply_to(std::span<const float> input, std::span<float> output) const noexcept;

private:
    WindowKind kind_;
    std::vector<float> samples_;
    float coherent_gain_ = 0.0f;
    float enbw_bins_ = 0.0f;
};

}  // namespace analyzer::dsp
