// Recovering an impulse response from a swept measurement.
//
// Play a known stimulus, record what comes back, and divide one by the other in
// the frequency domain. What falls out is the impulse response - everything the
// system did to the signal, in one time-domain trace, from which the frequency
// response, group delay, reverberation time and waterfall all follow.
//
// Why division needs regularising
//
// H = Y / X is exact and unusable. Wherever the stimulus has no energy - below
// its start frequency, above its end, in any notch - X approaches zero and the
// quotient explodes, turning measurement noise into enormous spurious content
// that swamps the real impulse.
//
// So the division is regularised:
//
//   H = Y * conj(X) / (|X|^2 + eps * max|X|^2)
//
// Where the stimulus is strong the epsilon term is negligible and this is plain
// division. Where it is weak, the denominator floors out and the result rolls
// gently to zero instead of blowing up. eps is the noise floor being assumed: at
// the 1e-6 default, anything more than 60 dB below the stimulus peak is treated
// as unmeasurable rather than amplified.
//
// Why an exponential sweep
//
// Any stimulus works here - that is the point of deconvolving rather than using
// a matched filter. But an exponential sweep has a property nothing else does:
// its harmonic distortion products appear at *negative* time, bunched before the
// linear impulse, so they can be windowed away or measured separately. A linear
// sweep smears them across the response with no way to separate them.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "dsp/complex.hpp"
#include "dsp/fft.hpp"

namespace analyzer::dsp {

// Default regularisation: treat anything 60 dB below the stimulus peak as noise.
inline constexpr float kDefaultRegularisation = 1e-6f;

// A recovered impulse response.
struct ImpulseResponse {
    // The response, at the measurement sample rate.
    std::vector<float> samples;
    // Index of the direct arrival, fractional from parabolic interpolation.
    float peak_samples = 0.0f;
    // Sample rate the measurement ran at.
    float sample_rate = 0.0f;

    // Time in seconds of sample `index`, relative to the direct arrival.
    float time_at(std::size_t index) const noexcept;

    // Peak absolute amplitude.
    float peak_amplitude() const noexcept;

    // Length in seconds.
    float duration_seconds() const noexcept;

    friend bool operator==(const ImpulseResponse&, const ImpulseResponse&) = default;
};

// Deconvolves a response against a stimulus.
//
// Sized at construction; deconvolve() allocates only the output, which is why
// it is not noexcept and not for the analysis thread. Not thread-safe: one
// instance belongs to one thread at a time.
class Deconvolver {
public:
    // Build a deconvolver for signals up to `max_length` samples.
    //
    // The transform is sized to the next power of two at or above twice
    // `max_length`, because circular convolution would otherwise wrap the tail
    // of the response around onto the start - reverberation folding back onto
    // the direct sound, which looks like a pre-echo that is not there.
    //
    // `max_length` must be non-zero and `sample_rate` must be positive.
    Deconvolver(float sample_rate, std::size_t max_length);

    // Transform length in use.
    std::size_t size() const noexcept { return size_; }

    // Longest input this deconvolver accepts.
    std::size_t max_length() const noexcept { return size_ / 2; }

    // Recover the impulse response.
    //
    // `regularisation` is the assumed noise floor as a fraction of stimulus peak
    // power; kDefaultRegularisation is a reasonable starting point. Larger
    // values suppress noise harder at the cost of accuracy where the stimulus
    // was weak.
    //
    // Returns nullopt if either signal is empty or longer than max_length(), or
    // if the stimulus is silent.
    std::optional<ImpulseResponse> deconvolve(std::span<const float> stimulus,
                                              std::span<const float> response,
                                              float regularisation = kDefaultRegularisation);

private:
    // Zero-pad `input` to the transform size and transform it into `target`.
    void transform(std::span<const float> input, std::vector<Complex32>& target) noexcept;

    std::size_t size_;
    float sample_rate_;
    RealFft fft_;
    std::vector<float> padded_;
    std::vector<Complex32> stimulus_spectrum_;
    std::vector<Complex32> response_spectrum_;
    std::vector<Complex32> quotient_;
    std::vector<float> result_;
};

}  // namespace analyzer::dsp
