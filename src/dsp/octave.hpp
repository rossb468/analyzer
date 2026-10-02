// Fractional-octave band analysis, following IEC 61260.
//
// An FFT gives linearly spaced bins; hearing works in octaves. Octave bands
// bridge the two by summing bin power into logarithmically spaced buckets, which
// is how noise measurements have been reported for decades and what makes two
// measurements of the same room comparable.
//
// Base-ten, not base-two
//
// IEC 61260 defines the octave ratio as G = 10^(3/10) ~= 1.9953, not exactly 2.
// The difference looks pedantic and is not: over the ten octaves of the audio
// band the two conventions drift far enough apart that band centres stop
// matching published tables, and a measurement that cannot be compared to
// anyone else's is worth much less.
//
// Bands narrower than the analysis
//
// At 48 kHz with a 4096-point FFT the bins are 11.7 Hz apart, while a
// third-octave band centred at 25 Hz is only 5.8 Hz wide. No FFT bin falls
// inside it, and no amount of arithmetic can recover what was never resolved.
// OctaveBands::is_resolvable() reports that honestly so a display can grey the
// band out, rather than printing a confident number derived from a neighbouring
// bin.

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <vector>

namespace analyzer::dsp {

// The IEC 61260 octave ratio, 10^(3/10).
inline constexpr float kOctaveRatio = 1.9952623f;

// Floor for a band with no energy.
inline constexpr float kBandFloorDb = -200.0f;

// One frequency band.
struct Band {
    // Exact centre frequency.
    float centre_hz = 0.0f;
    // Lower band edge.
    float lower_hz = 0.0f;
    // Upper band edge.
    float upper_hz = 0.0f;

    // Width in hertz.
    float width_hz() const noexcept { return upper_hz - lower_hz; }

    // The nominal frequency this band is known by.
    //
    // Exact centres are awkward numbers - a third-octave band sits at 1258.9 Hz
    // and everyone calls it 1250. This rounds to the preferred series so labels
    // match what is printed on every other analyser.
    float nominal_hz() const noexcept;

    friend constexpr bool operator==(const Band&, const Band&) = default;
};

// A set of fractional-octave bands.
class OctaveBands {
public:
    // Build bands of 1/fraction octave covering min_hz..max_hz. `fraction` must
    // be at least 1, and the range positive and ascending.
    OctaveBands(std::uint32_t fraction, float min_hz, float max_hz);

    // Third-octave bands across the audio band, the usual default.
    static OctaveBands third_octave() { return OctaveBands(3, 20.0f, 20'000.0f); }

    // The bands, ascending.
    std::span<const Band> bands() const noexcept { return bands_; }

    // How many bands there are.
    std::size_t size() const noexcept { return bands_.size(); }

    bool empty() const noexcept { return bands_.empty(); }

    // The fraction these bands divide an octave into.
    std::uint32_t fraction() const noexcept { return fraction_; }

    // Whether band `index` is wide enough for the analysis to resolve.
    //
    // A band narrower than the bin spacing contains no bin, and a level
    // reported for it would be borrowed from a neighbour rather than measured.
    // An index past the end is not resolvable.
    bool is_resolvable(std::size_t index, float bin_spacing_hz) const noexcept;

    // The lowest band the analysis can actually resolve.
    //
    // A UI wanting a single cut-off rather than a per-band check can start here.
    std::optional<std::size_t> first_resolvable(float bin_spacing_hz) const noexcept;

    // Sum a spectrum into bands.
    //
    // `bins_db` holds one level per FFT bin, bin k at k * bin_spacing_hz. Power
    // is summed within each band and converted back to decibels, which is the
    // correct operation - adding decibels directly would be a geometric mean and
    // read low wherever a band is peaky.
    //
    // Bands too narrow to hold a bin take the nearest bin's level.
    // is_resolvable() says which those are. With no usable bins, or a bin
    // spacing that is not positive, every band reads kBandFloorDb.
    //
    // `out` must hold exactly one element per band. Allocates nothing.
    void apply(std::span<const float> bins_db, float bin_spacing_hz,
               std::span<float> out) const noexcept;

    friend bool operator==(const OctaveBands&, const OctaveBands&) = default;

private:
    std::uint32_t fraction_;
    std::vector<Band> bands_;
};

}  // namespace analyzer::dsp
