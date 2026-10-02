// Harmonic distortion from a spectrum containing a steady tone.
//
// Feed a sine through a system, look at what comes back, and everything that is
// not the sine is something the system added. The harmonics say what kind of
// nonlinearity it is - a symmetric one like clipping produces odd orders, an
// asymmetric one produces even - and the total says how much.
//
// Summing lobes, not bins
//
// A tone almost never lands exactly on a bin centre, and a windowed tone that
// does not spreads across its window's main lobe. Reading a single bin
// therefore under-reads by up to the window's scalloping loss, which for Hann is
// 1.4 dB and swings with frequency. Every peak here is summed across its lobe
// instead, which is why a measured harmonic level does not drift as the
// fundamental moves between bins.
//
// Harmonics above Nyquist
//
// At 48 kHz a 5 kHz fundamental has its fifth harmonic at 25 kHz, above
// Nyquist. It does not vanish: it aliases back to 23 kHz, where reading it would
// report a completely fictional distortion product. Orders beyond Nyquist are
// excluded and Distortion::orders_above_nyquist says how many, so a display can
// show "THD (to H4)" rather than implying it measured all ten.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace analyzer::dsp {

// Floor for decibel output.
inline constexpr float kDistortionFloorDb = -200.0f;

// One harmonic of the fundamental.
struct Harmonic {
    // Harmonic number: 2 is the octave above the fundamental.
    std::uint32_t order = 0;
    // Where it was actually found, which may differ slightly from order * f0.
    float hz = 0.0f;
    // Absolute level in the same reference as the input spectrum.
    float level_db = 0.0f;
    // Level relative to the fundamental. Always negative for a sane system.
    float relative_db = 0.0f;
    // The same ratio as a percentage of the fundamental's amplitude.
    float percent = 0.0f;

    friend bool operator==(const Harmonic&, const Harmonic&) = default;
};

// A distortion measurement.
struct Distortion {
    // Fundamental frequency, refined from the spectrum.
    float fundamental_hz = 0.0f;
    // Fundamental level.
    float fundamental_db = 0.0f;
    // Harmonics found, ascending by order.
    std::vector<Harmonic> harmonics;
    // Total harmonic distortion as a percentage of the fundamental.
    float thd_percent = 0.0f;
    // The same figure in decibels relative to the fundamental.
    float thd_db = 0.0f;
    // Total harmonic distortion plus noise: everything that is not the
    // fundamental, including hum, hiss and any non-harmonic product.
    float thd_n_percent = 0.0f;
    // The same figure in decibels.
    float thd_n_db = 0.0f;
    // Median level of the bins that are neither fundamental nor harmonic.
    //
    // A median rather than a mean, because a mean is dragged upwards by any
    // spur the analysis did not classify.
    float noise_floor_db = 0.0f;
    // Harmonic orders that would fall above Nyquist and were therefore not
    // measured. Non-zero means the reported THD covers fewer orders than asked
    // for.
    std::uint32_t orders_above_nyquist = 0;

    // Highest harmonic order actually measured; 1 when there are none.
    std::uint32_t highest_order() const noexcept;

    // A harmonic by order, if it was measured.
    std::optional<Harmonic> harmonic(std::uint32_t order) const;

    // A label honest about how many orders the figure covers.
    std::string thd_label() const;

    friend bool operator==(const Distortion&, const Distortion&) = default;
};

// How to look for harmonics.
struct DistortionConfig {
    // Highest harmonic order to look for.
    std::uint32_t max_order = 10;
    // How far either side of order * f0 to search for the peak, in bins.
    //
    // Needed because a real system's harmonics sit exactly at multiples of the
    // fundamental, but the *measured* fundamental has bin-quantisation error
    // that multiplies with order - a half-bin error on f0 is a five-bin error
    // on H10.
    std::size_t search_bins = 4;
    // How many bins either side of a peak to sum, covering the window's main
    // lobe.
    std::size_t lobe_bins = 3;

    friend bool operator==(const DistortionConfig&, const DistortionConfig&) = default;
};

// Measure distortion in a spectrum.
//
// `power_bins` is **linear mean-square power per bin**, as produced by the
// spectrum analyzer's power(), not decibels. Summing decibels would be a
// geometric mean and wrong by several dB on anything peaky.
//
// `fundamental_hz` may be given when it is known; otherwise the loudest bin is
// used. Supplying it matters when the distortion is severe enough that a
// harmonic rivals the fundamental.
//
// Returns nullopt if the spectrum is empty, the bin spacing is not positive, or
// no fundamental can be found.
//
// Allocates, so it belongs on a UI or analysis-request path, not in a per-block
// loop.
std::optional<Distortion> analyse_distortion(std::span<const float> power_bins,
                                             float bin_spacing_hz,
                                             std::optional<float> fundamental_hz,
                                             const DistortionConfig& config);

}  // namespace analyzer::dsp
