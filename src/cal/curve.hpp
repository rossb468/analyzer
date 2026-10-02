// Frequency response correction curves.
//
// A measurement microphone ships with a calibration file: a list of frequencies
// and the decibels by which that specific capsule deviates from flat. Applying
// it is the difference between measuring the room and measuring the room plus
// the microphone.
//
// Interpolation is linear in dB against **log** frequency. Calibration files are
// sparse and roughly log-spaced - a few dozen points across three decades - so
// interpolating against linear frequency would badly misplace everything below
// a few hundred hertz.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace analyzer::cal {

// One row of a calibration file.
struct CurvePoint {
    float hz = 0.0f;
    float db = 0.0f;

    friend bool operator==(const CurvePoint&, const CurvePoint&) = default;
};

// A sparse frequency response, sorted ascending by frequency.
class ResponseCurve {
public:
    // A curve that corrects nothing.
    ResponseCurve() = default;

    // Build from points.
    //
    // Points are sorted and non-positive or non-finite entries dropped, so a
    // file listing a DC row or arriving out of order still works. Of several
    // points at the same frequency the first listed wins.
    explicit ResponseCurve(std::vector<CurvePoint> points);

    // A curve that corrects nothing.
    static ResponseCurve flat() { return ResponseCurve(); }

    // Parse a calibration file.
    //
    // Accepts the format every microphone vendor and REW uses: comment lines
    // starting with `*`, `#`, `;` or `"`, then whitespace or comma separated
    // `frequency level [phase]`. Phase is ignored; magnitude correction is what
    // a capsule file is for.
    //
    // Unparseable lines are skipped rather than failing the whole file, because
    // vendor files routinely carry stray headers and trailing junk. That is why
    // this does not throw: there is no input it rejects, and a file with no
    // usable rows is simply a flat curve.
    static ResponseCurve parse(std::string_view contents);

    // Whether this curve would change anything.
    bool is_flat() const noexcept { return points_.empty(); }

    // The points, ascending by frequency.
    const std::vector<CurvePoint>& points() const noexcept { return points_; }

    // Correction in decibels at `hz`.
    //
    // Outside the curve's range the nearest endpoint is held rather than
    // extrapolated. Extrapolating a microphone's response past where it was
    // actually measured invents data, and the error grows fastest exactly where
    // the curve is steepest.
    //
    // A flat curve, or a frequency that is not positive and finite, reads 0.
    float db_at(float hz) const noexcept;

    friend bool operator==(const ResponseCurve&, const ResponseCurve&) = default;

private:
    std::vector<CurvePoint> points_;
};

// Summary for display: "flat", or "5 points, 20-20000 Hz".
std::string to_string(const ResponseCurve& curve);

}  // namespace analyzer::cal
