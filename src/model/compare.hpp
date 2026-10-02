// Comparing two frequency responses.
//
// Built for the REW parity run, which is the project's one unmet commitment:
// feed identical input through this analyser and through REW, and match its
// exported magnitude to +-0.1 dB on synthetic signals and +-0.5 dB on a real
// measurement, 20 Hz to 20 kHz.
//
// Why a constant offset is reported separately
//
// Two analysers can disagree for two very different reasons, and lumping them
// together wastes the run.
//
// A constant offset across the whole band is a reference convention. 0 dBFS =
// full-scale sine and 0 dBFS = full-scale square differ by 3.01 dB and both are
// defensible; so does reporting per-bin level against power per hertz, which
// differs by the window's noise bandwidth. None of that is a defect, and a run
// that fails on it tells you nothing you did not already know.
//
// A frequency-dependent deviation is the thing worth finding: a window
// amplitude correction applied where it should not be, an FFT scaling that
// forgot a factor of two, a calibration constant. That is what
// Comparison::max_deviation_after_offset isolates, and it is the number the exit
// criterion should be read against once the conventions are reconciled.
//
// Interpolation
//
// The two files rarely share a frequency grid. The reference is interpolated
// onto the subject's frequencies, linearly in decibels against log frequency,
// matching how every other sparse curve in this codebase is read.
//
// Interpolation is itself a source of error, and near a sharp peak it can
// exceed the tolerance being tested. Comparison::interpolated reports how many
// points needed it, so a run that is mostly interpolation can be recognised as
// one rather than believed.

#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace analyzer::model {

// One row of a response: a frequency and a level.
struct ResponsePoint {
    double hz = 0.0;
    double db = 0.0;

    friend bool operator==(const ResponsePoint&, const ResponsePoint&) = default;
};

// A frequency response read from a text file.
struct Response {
    // Sorted ascending by frequency.
    std::vector<ResponsePoint> points;

    // Parse `frequency level` rows from text.
    //
    // Deliberately forgiving about shape, because the whole point is reading
    // another application's export: comment markers (`*`, `#`, `;`, `/`), blank
    // lines and header rows are skipped, and columns may be separated by tabs,
    // commas, semicolons or spaces. A third column, which REW uses for phase, is
    // ignored.
    //
    // Rows that are not two numbers are skipped rather than fatal. An export
    // with a stray legend in the middle should still compare. That is why this
    // does not throw: there is no input it rejects.
    static Response parse(std::string_view contents);

    // Level at `hz`, interpolated in decibels against log frequency.
    //
    // Returns nullopt outside the covered range rather than extrapolating: a
    // file that stops at 20 kHz says nothing about 22 kHz, and inventing a value
    // there would put fabricated data into a parity result.
    std::optional<double> level_at(double hz) const;

    // Whether a frequency falls exactly on a listed point.
    bool has_exact(double hz) const;

    friend bool operator==(const Response&, const Response&) = default;
};

// The result of comparing two responses over a band.
struct Comparison {
    // Points compared.
    std::size_t compared = 0;
    // How many of those needed the reference to be interpolated.
    std::size_t interpolated = 0;
    // Largest absolute difference, in decibels.
    double max_deviation = 0.0;
    // Frequency at which that occurred.
    double max_deviation_hz = 0.0;
    // Root-mean-square difference, in decibels.
    double rms_deviation = 0.0;
    // Mean difference: the constant offset between the two.
    double mean_offset = 0.0;
    // Largest absolute difference once mean_offset is removed.
    double max_deviation_after_offset = 0.0;
    // Frequency at which that occurred.
    double max_deviation_after_offset_hz = 0.0;
    // Root-mean-square difference once mean_offset is removed.
    double rms_deviation_after_offset = 0.0;
    // Low end of the band compared.
    double from_hz = 0.0;
    // High end of the band compared.
    double to_hz = 0.0;

    // Whether the shapes agree to `tolerance`, ignoring a constant offset.
    //
    // This is what the parity criterion should be read against once the
    // reference conventions have been reconciled, because a convention mismatch
    // is not a defect.
    bool agrees_within(double tolerance) const noexcept {
        return compared > 0 && max_deviation_after_offset <= tolerance;
    }

    // A human-readable report.
    std::string report() const;

    friend bool operator==(const Comparison&, const Comparison&) = default;
};

// Compare `subject` against `reference` over [from_hz, to_hz].
//
// Every point of `subject` inside the band is compared against `reference`
// interpolated to the same frequency. Points the reference does not cover are
// skipped rather than counted as agreement.
Comparison compare(const Response& subject, const Response& reference, double from_hz,
                   double to_hz);

}  // namespace analyzer::model
