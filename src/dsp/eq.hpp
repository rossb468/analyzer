// Equalisation: a cascade of second-order sections.
//
// Two shapes of the same thing. A graphic equaliser is a fixed set of bands at
// standard centre frequencies where only the gains move; a parametric
// equaliser lets every band choose its own frequency, gain, Q and type. They
// share one implementation because they are one thing - a graphic EQ is a
// parametric EQ with the other controls nailed down - and keeping them
// separate is how the two would drift apart.
//
// What this is for
//
// Two jobs, and they are different:
//
// - Prediction. Take a measured room response and show what it would look like
//   after a filter, before committing to anything. This is the common case and
//   needs only Equaliser::response_at(), which is arithmetic on the
//   coefficients and touches no audio.
// - Application. Actually filter a signal. Needs Equaliser::process() and
//   carries per-band state.
//
// Prediction is deliberately separate from application, so a UI can draw a
// curve for a filter that is not running.
//
// Gain staging
//
// Bands add. Ten bands at +6 dB is +60 dB in the region where they overlap,
// and that clips. Equaliser::peak_gain_db() reports the worst case so a UI can
// show it and a caller can trim by it; nothing here applies that trim
// automatically, because silently changing a level the user set is worse than
// showing them the number.

#pragma once

#include <array>
#include <cstddef>
#include <numbers>
#include <span>
#include <vector>

#include "dsp/biquad.hpp"
#include "dsp/complex.hpp"

namespace analyzer::dsp {

// Standard ISO octave centres for a ten-band graphic equaliser.
//
// These are the preferred frequencies from IEC 61260, rounded the way every
// graphic EQ has printed them since the 1970s. Using the exact base-ten values
// instead would be more correct and would surprise everybody.
inline constexpr std::array<float, 10> kOctaveCentres = {
    31.5f, 63.0f, 125.0f, 250.0f, 500.0f, 1000.0f, 2000.0f, 4000.0f, 8000.0f, 16000.0f,
};

// Q for a peaking filter one octave wide.
//
// Exactly the square root of two, which is not a coincidence: the cookbook's
// 1/Q = 2*sinh(ln(2)/2 * BW) at BW = 1 gives
// 2 * (sqrt(2) - 1/sqrt(2)) / 2 = 1/sqrt(2).
//
// Ganged faders overshoot. With every band at +4 dB the combined curve reads
// about +5.8 dB, because octave-spaced peaking filters overlap and decibels
// add. That is inherent to a constant-Q graphic equaliser rather than a
// defect; every hardware unit of this shape does it. Anyone using the faders
// as a broad tilt should expect the curve to exceed the fader marks, and
// should check Equaliser::peak_gain_db().
//
// A sweep from Q 1.0 to 4.1 puts the flattest ganged response near 1.5 rather
// than here, but only by 0.18 dB. Interoperability wins that trade: a band
// exported to another tool has to mean one octave there.
inline constexpr float kOctaveQ = std::numbers::sqrt2_v<float>;

// What shape a band has.
enum class FilterKind {
    // A bump or dip centred on the frequency. The workhorse.
    Peaking,
    // Everything below the frequency lifted or cut.
    LowShelf,
    // Everything above the frequency lifted or cut.
    HighShelf,
    // Rolls off above the frequency. Gain is ignored.
    LowPass,
    // Rolls off below the frequency. Gain is ignored.
    HighPass,
    // Passes a band, rejects either side. Gain is ignored.
    BandPass,
    // A null at the frequency. Gain is ignored.
    Notch,
    // Flat magnitude, rotated phase. Gain is ignored.
    AllPass,
};

// Whether `gain_db` means anything for this shape.
//
// A UI should grey the gain control out when this is false rather than letting
// someone set a number that does nothing.
bool uses_gain(FilterKind kind) noexcept;

// Short label, matching what the rest of the industry prints.
const char* label(FilterKind kind) noexcept;

// Full name, for test failure messages and logs.
const char* to_string(FilterKind kind) noexcept;

// One band of an equaliser.
struct FilterBand {
    FilterKind kind = FilterKind::Peaking;
    // Centre or corner frequency in hertz.
    float hz = 1000.0f;
    // Gain in decibels. Ignored unless uses_gain(kind).
    float gain_db = 0.0f;
    // Quality factor. Higher is narrower.
    float q = kOctaveQ;
    // Whether the band contributes. A disabled band keeps its settings.
    bool enabled = true;

    // A peaking band.
    static constexpr FilterBand peaking(float hz, float gain_db, float q) {
        return {FilterKind::Peaking, hz, gain_db, q, true};
    }

    // Whether this band changes anything.
    //
    // A peaking band at 0 dB is exactly a pass-through, and skipping it saves
    // a section - which matters when a ten-band EQ usually has two bands set.
    bool is_transparent() const noexcept;

    // Design the section this band describes.
    Biquad design(float sample_rate) const noexcept;

    friend constexpr bool operator==(const FilterBand&, const FilterBand&) = default;
};

// A cascade of bands.
//
// Coefficients are designed once, when a band changes, not per sample and not
// per query. A ten-band curve evaluated across 2048 display points is 20,480
// complex divisions; redesigning the sections inside that loop would make it
// twenty times more.
//
// Not thread-safe: process() mutates the sections' delay lines, so one
// instance belongs to one thread at a time. A UI that only predicts can own a
// separate copy.
class Equaliser {
public:
    // An equaliser with the given bands.
    Equaliser(float sample_rate, std::vector<FilterBand> bands);

    // A ten-band graphic equaliser on ISO octave centres, all flat.
    static Equaliser graphic(float sample_rate);

    // An empty parametric equaliser.
    static Equaliser parametric(float sample_rate);

    std::span<const FilterBand> bands() const noexcept { return bands_; }
    float sample_rate() const noexcept { return sample_rate_; }
    float preamp_db() const noexcept { return preamp_db_; }

    // Set the output trim, in decibels. A non-finite value becomes 0.
    void set_preamp_db(float db) noexcept;

    // Replace a band. Out-of-range indices are ignored.
    void set_band(std::size_t index, const FilterBand& band) noexcept;

    // Set just a band's gain, which is all a graphic equaliser can do.
    // Out-of-range indices are ignored.
    void set_gain_db(std::size_t index, float gain_db) noexcept;

    // Append a band and return its index.
    std::size_t push_band(const FilterBand& band);

    // Remove a band. Out-of-range indices are ignored.
    void remove_band(std::size_t index);

    // Change the sample rate, redesigning every section.
    //
    // A band above the new Nyquist becomes a pass-through rather than an
    // error: a preset written at 96 kHz is still mostly useful at 44.1, and
    // refusing the whole thing over its top band would help nobody.
    void set_sample_rate(float sample_rate);

    // Set every band at once.
    void set_bands(std::vector<FilterBand> bands);

    // Complex response of the whole cascade at `hz`, including the preamp.
    //
    // Sections multiply, which is the entire reason a cascade is the right
    // structure: magnitudes in decibels add and phases add, so the combined
    // curve is the sum of the individual ones on a log plot.
    Complex32 response_at(float hz) const noexcept;

    // Magnitude of the whole cascade at `hz`, in decibels.
    float magnitude_db_at(float hz) const noexcept;

    // Magnitude of one band at `hz`, in decibels. An out-of-range index reads
    // as 0 dB.
    //
    // For drawing the individual band curves under the combined one, which is
    // what makes a parametric EQ possible to reason about.
    float band_magnitude_db_at(std::size_t index, float hz) const noexcept;

    // Write the combined magnitude, in decibels, for each frequency. If the
    // spans differ in length, the shorter one decides how many are written.
    void write_magnitude_db(std::span<const float> frequencies,
                            std::span<float> out) const noexcept;

    // Write the combined phase, in degrees, for each frequency. If the spans
    // differ in length, the shorter one decides how many are written.
    void write_phase_degrees(std::span<const float> frequencies,
                             std::span<float> out) const noexcept;

    // The largest gain the cascade applies anywhere in the audio band.
    //
    // Bands add. Ten bands at +6 dB is +60 dB where they overlap, and that
    // clips. A UI showing this next to a "Trim" button is the difference
    // between an equaliser that is safe to use and one that is not.
    //
    // Sampled on a log grid rather than solved analytically: a peak between
    // two grid points differs from the true maximum by well under the 0.1 dB
    // anyone can act on, and the closed form for an arbitrary cascade is not
    // worth having.
    float peak_gain_db() const noexcept;

    // Set the preamp so the cascade's loudest point sits at unity.
    //
    // Never automatic. A caller asks for this explicitly, because quietly
    // moving a level the user set is worse than showing them the number and
    // letting them decide.
    void trim_to_unity() noexcept;

    // Filter a block in place.
    void process(std::span<float> samples) noexcept;

    // Clear every section's delay line.
    void reset() noexcept;

    // Set every gain to zero, leaving frequencies and Qs alone.
    void flatten() noexcept;

private:
    // Resize the sections to match the bands and redesign every one.
    void redesign();

    std::vector<FilterBand> bands_;
    std::vector<Biquad> sections_;
    float sample_rate_;
    // Applied after the cascade, for trimming the gain the bands added.
    float preamp_db_ = 0.0f;
};

}  // namespace analyzer::dsp
