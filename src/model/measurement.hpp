// The model module: session state, the measurement store and the on-disk
// format.
//
// The plan calls for designing the container before there is data to migrate,
// because retrofitting a format after users have a folder of saved measurements
// is the worst version of that problem. So this exists ahead of anything that
// writes to it. What is here:
//
//   measurement.hpp     what a stored measurement is (this file)
//   store.hpp           an ordered, in-memory collection of them
//   format.hpp          the .anlz container: text header, raw f64 block
//   export.hpp          REW-compatible text export
//   filter_export.hpp   equaliser export for REW, Equalizer APO and miniDSP
//   settings.hpp        program preferences, and their file
//   wav.hpp             WAV reading and writing
//   compare.hpp         comparing two exported responses, for the REW parity run
//   error.hpp           the exceptions all of the above throw
//
// Text written here is byte-for-byte what the Rust core wrote for the same
// input, number formatting included, so that files move freely between the two
// implementations and the exports can be checked against fixtures.
//
// Storage rules (what a stored measurement is)
//
// Three decisions here are load-bearing, and all three are cheap now and
// impossible to retrofit once someone has a folder of saved measurements.
//
// Unsmoothed, at the native sample rate, complex. Smoothing and
// fractional-octave banding are *view* transforms. Storing a smoothed
// magnitude curve throws away the phase and the resolution, which permanently
// forecloses group delay, RT60, minimum-phase decomposition and any meaningful
// interop.
//
// f64, not f32. Analysis runs in single precision because that is what
// converters deliver and what SIMD likes. Storage is different: a file is read
// back, processed, and written again, possibly many times, and rounding at
// every round trip accumulates. Doubling the file size is a trivial price.
//
// Absolute references travel with the data. Time zero, full-scale voltages,
// reference resistance, SPL offset. Without them a measurement cannot be
// compared with another, converted to real units, or aligned in time - it
// becomes a picture of a measurement rather than a measurement.

#pragma once

#include <compare>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace analyzer::model {

// A complex value in storage precision.
using Complex64 = std::complex<double>;

// Identifier for a measurement within a session.
struct MeasurementId {
    std::uint64_t value = 0;

    friend constexpr auto operator<=>(const MeasurementId&, const MeasurementId&) = default;
};

// "#7", for display.
std::string to_string(MeasurementId id);

// Absolute references that make a measurement comparable and convertible.
//
// Every field is optional because an uncalibrated measurement is a legitimate
// thing to have. nullopt means "not known", which is different from zero and
// must not be silently substituted for it.
struct References {
    // Decibels added to dBFS to obtain dB SPL.
    std::optional<double> spl_offset_db;
    // RMS volts that read full scale on the input converter.
    std::optional<double> full_scale_input_volts;
    // RMS volts the output converter produces at full scale.
    std::optional<double> full_scale_output_volts;
    // Series resistance used for impedance measurement, in ohms.
    std::optional<double> reference_resistance_ohms;
    // Acoustic propagation delay already removed from the data, in seconds.
    //
    // Recording this matters as much as removing it: two measurements aligned
    // by different amounts cannot be compared, and there is no way to tell
    // after the fact unless the amount was written down.
    std::optional<double> propagation_delay_seconds;

    // Whether nothing at all is known.
    bool is_empty() const noexcept { return *this == References{}; }

    friend bool operator==(const References&, const References&) = default;
};

// A power spectrum, stored as complex bins so phase survives.
struct SpectrumData {
    // One complex value per bin, bin k at k * bin_spacing_hz.
    std::vector<Complex64> bins;
    // Hertz between bins.
    double bin_spacing_hz = 0.0;

    friend bool operator==(const SpectrumData&, const SpectrumData&) = default;
};

// An impulse response in the time domain.
struct ImpulseResponseData {
    // Samples at the measurement's sample rate.
    std::vector<double> samples;
    // Index of t = 0, which is fractional because the direct arrival rarely
    // lands on a sample. Rounding it away costs sub-sample alignment and, at
    // 48 kHz, 7 mm of path length per sample.
    double time_zero_samples = 0.0;

    friend bool operator==(const ImpulseResponseData&, const ImpulseResponseData&) = default;
};

// A magnitude-only spectrum, as an RTA produces.
//
// Deliberately a separate type rather than a SpectrumData with zero imaginary
// parts. A power spectrum discards phase when it squares the magnitude - there
// is no phase to store, and writing zeros would be indistinguishable from
// having measured zero phase. Later code would believe it.
struct PowerSpectrumData {
    // Level per bin in decibels.
    std::vector<double> magnitude_db;
    // Hertz between bins.
    double bin_spacing_hz = 0.0;

    friend bool operator==(const PowerSpectrumData&, const PowerSpectrumData&) = default;
};

// A two-channel transfer function with its coherence.
struct TransferFunctionData {
    // Complex response per bin.
    std::vector<Complex64> bins;
    // Coherence per bin, 0..=1. Kept alongside because a response without it
    // cannot be judged - there is no way to tell which parts to believe.
    std::vector<double> coherence;
    // Hertz between bins.
    double bin_spacing_hz = 0.0;

    friend bool operator==(const TransferFunctionData&, const TransferFunctionData&) = default;
};

// The payload of a measurement. The alternatives carry genuinely different
// data, which is why this is a variant rather than a struct with most of its
// fields meaningless.
using MeasurementData =
    std::variant<SpectrumData, ImpulseResponseData, PowerSpectrumData, TransferFunctionData>;

// A short label for the kind of data: "spectrum", "power_spectrum",
// "impulse_response" or "transfer_function". These are written to files, so
// they never change.
std::string_view kind(const MeasurementData& data) noexcept;

// Number of stored points.
std::size_t point_count(const MeasurementData& data) noexcept;

// Whether there is no data.
bool is_empty(const MeasurementData& data) noexcept;

// Bin spacing, for frequency-domain data.
std::optional<double> bin_spacing_hz(const MeasurementData& data) noexcept;

// Magnitude in decibels per bin, for frequency-domain data.
//
// A view, computed on demand. Deliberately not stored - see the storage rules.
// A bin of exactly zero reads -200 rather than minus infinity.
std::optional<std::vector<double>> magnitude_db(const MeasurementData& data);

// Phase in degrees per bin, for frequency-domain data that has any.
//
// A power spectrum reports none: no phase was ever measured.
std::optional<std::vector<double>> phase_degrees(const MeasurementData& data);

// One saved measurement.
struct Measurement {
    // Identifier within its session.
    MeasurementId id;
    // Name shown to the user.
    std::string name;
    // Free-text notes.
    std::string notes;
    // Seconds since the Unix epoch when this was captured.
    //
    // A plain number rather than a time_point, because it has to survive a
    // round trip through a file and back without depending on a clock type.
    std::int64_t captured_at = 0;
    // Rate the measurement was taken at. Never assumed.
    double sample_rate = 0.0;
    // How many input channels contributed.
    std::size_t channels = 1;
    // Absolute references, so far as they are known.
    References references;
    // The data itself.
    MeasurementData data;

    // Build a measurement with empty metadata.
    Measurement(MeasurementId identifier, std::string display_name, double rate,
                MeasurementData payload);

    // Frequency of bin `index`, for frequency-domain data.
    std::optional<double> bin_frequency(std::size_t index) const noexcept;

    // Duration in seconds, for time-domain data.
    std::optional<double> duration_seconds() const noexcept;

    // Whether an SPL offset is recorded, meaning levels can be shown as SPL
    // rather than dBFS.
    bool is_spl_calibrated() const noexcept { return references.spl_offset_db.has_value(); }

    friend bool operator==(const Measurement&, const Measurement&) = default;
};

}  // namespace analyzer::model

template <>
struct std::hash<analyzer::model::MeasurementId> {
    std::size_t operator()(analyzer::model::MeasurementId id) const noexcept {
        return std::hash<std::uint64_t>{}(id.value);
    }
};
