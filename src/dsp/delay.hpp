// Finding the propagation delay between reference and measurement.
//
// A microphone five metres from a loudspeaker hears everything about fifteen
// milliseconds late. Left uncompensated that wrecks a transfer function: a
// constant delay is a phase that rotates ever faster with frequency, and within
// one analysis frame the two channels stop lining up, so coherence collapses at
// the top of the band. Finding and removing the delay is a prerequisite, not a
// refinement.
//
// Why PHAT
//
// Plain cross-correlation weights each frequency by how much energy it carries,
// so a real room - where the bass is loud and reverberant - produces a broad,
// smeared peak sitting on a forest of reflections. The phase transform (GCC-PHAT)
// divides the cross-spectrum by its own magnitude, keeping only phase. Every
// frequency then contributes equally and the direct-sound peak becomes sharp and
// unambiguous. It is the difference between a delay finder that works in an
// anechoic chamber and one that works in a room.
//
// Plain correlation is still available, because with a very poor
// signal-to-noise ratio PHAT's equal weighting amplifies bins that are pure
// noise.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <vector>

#include "dsp/complex.hpp"
#include "dsp/fft.hpp"

namespace analyzer::dsp {

// How the cross-spectrum is weighted before transforming back.
enum class Weighting {
    // Phase transform. Whitens the cross-spectrum so every frequency counts
    // equally, giving a sharp peak in a reverberant space.
    Phat,
    // No weighting. Better when noise dominates, worse when reflections do.
    None,
};

// Speed of sound in air, 343 m/s, the conventional figure at 20 C. Turns a
// delay into a distance.
inline constexpr float kSpeedOfSound = 343.0f;

// What the finder concluded.
struct DelayEstimate {
    // Delay in samples. Positive means the measurement arrived *after* the
    // reference, which is the normal case for a microphone at a distance.
    float samples = 0.0f;
    // The same delay in seconds.
    float seconds = 0.0f;
    // Height of the correlation peak relative to the mean, a measure of how
    // sharp and isolated it was. Around 1 means no peak at all; a clean direct
    // arrival gives tens or hundreds.
    float confidence = 0.0f;

    // Distance the sound travelled, at kSpeedOfSound.
    //
    // Only meaningful for an acoustic path; an electrical loopback delay is not
    // a distance.
    float metres() const noexcept { return seconds * kSpeedOfSound; }

    friend constexpr bool operator==(const DelayEstimate&, const DelayEstimate&) = default;
};

// Finds the delay between two signals by cross-correlation.
//
// All buffers are sized at construction, so find() neither allocates nor
// throws and is safe on the analysis thread. Not thread-safe: one instance
// belongs to one thread at a time.
class DelayFinder {
public:
    // Build a finder over a correlation window of `size` samples.
    //
    // The largest delay that can be found is `size / 2` in either direction,
    // because beyond that the circular correlation wraps and a late arrival is
    // indistinguishable from an early one. At 48 kHz, 16384 covers +-170 ms,
    // which is far more than any sane acoustic path.
    //
    // `size` must be even and at least four, and `sample_rate` must be positive.
    DelayFinder(float sample_rate, std::size_t size, Weighting weighting);

    // A finder with sensible defaults for acoustic work.
    static DelayFinder acoustic(float sample_rate);

    // Correlation window length.
    std::size_t size() const noexcept { return size_; }

    // Largest delay this finder can resolve, in either direction.
    std::size_t max_delay_samples() const noexcept { return size_ / 2; }

    // Estimate the delay from `reference` to `measurement`.
    //
    // Both spans are truncated or zero-padded to the correlation size. Returns
    // nullopt when either signal is silent, since a delay between nothing and
    // nothing is not a number.
    std::optional<DelayEstimate> find(std::span<const float> reference,
                                      std::span<const float> measurement) noexcept;

private:
    // The correlation's peak, refined to a fraction of a sample. Never empty:
    // the correlation always holds `size()` values.
    DelayEstimate locate_peak() const noexcept;

    float sample_rate_;
    std::size_t size_;
    Weighting weighting_;
    RealFft fft_;

    std::vector<float> padded_reference_;
    std::vector<float> padded_measurement_;
    std::vector<Complex32> reference_spectrum_;
    std::vector<Complex32> measurement_spectrum_;
    std::vector<Complex32> cross_;
    std::vector<float> correlation_;
};

}  // namespace analyzer::dsp
