// Reducing a spectrum to one value per pixel column.
//
// An FFT produces linearly-spaced bins; a log frequency axis needs points
// spaced logarithmically. The mismatch runs in both directions at once, which
// is what makes this more than a resample:
//
// - High frequencies are over-sampled. At 48 kHz with a 4096-point FFT the
//   top octave holds over a thousand bins inside maybe 150 pixels. Several bins
//   land in every column and have to be combined.
// - Low frequencies are under-sampled. Below a couple of hundred hertz the
//   bins are further apart than the pixels, so most columns contain no bin at
//   all and have to be interpolated.
//
// Getting either case wrong is visible. Picking one bin per column in the dense
// region makes narrow peaks flicker in and out as the display resizes; leaving
// empty columns in the sparse region draws a staircase in the bass.
//
// Both reductions run on the draw path, every frame. They write into a Trace
// the caller keeps and recycles, so at a steady width they allocate nothing.

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "plot/axis.hpp"

namespace analyzer::plot {

// How to combine several bins landing in one pixel column.
enum class Reduction {
    // Loudest bin in the column.
    //
    // The default, because losing a narrow peak is the more visible error. A
    // resonance one bin wide is exactly what a measurement is looking for, and
    // averaging it away in the display would hide it.
    Max,
    // Mean level across the column, averaged in the power domain.
    //
    // Averaging decibels directly is wrong - it is a geometric mean of power
    // and reads several dB low on a peaky spectrum - so this converts, averages
    // and converts back.
    Mean,
};

// One reduced trace, ready to be turned into geometry.
struct Trace {
    // One level per pixel column, in decibels.
    std::vector<float> points;

    // A trace sized for `width` columns, every one negative infinity (no data).
    static Trace with_width(std::size_t width);

    // Number of columns.
    std::size_t size() const noexcept { return points.size(); }

    // Whether the trace has no columns.
    bool empty() const noexcept { return points.empty(); }

    friend bool operator==(const Trace&, const Trace&) = default;
};

// Reduce `bins` onto the axis, writing one value per pixel column.
//
// `bins` are levels in decibels, bin k sitting at k * bin_spacing_hz. The
// output is resized to `columns`; it allocates only if that exceeds the
// capacity `out` already has, so recycle one Trace across frames. Columns that
// get no data read negative infinity, which is also all of them when `bins` has
// fewer than two entries, the spacing is not positive, or `columns` is zero.
//
// Bin 0 is skipped: it is DC, has no place on a log frequency axis, and would
// otherwise be smeared across the leftmost column.
void reduce(std::span<const float> bins, float bin_spacing_hz, const FrequencyAxis& axis,
            std::size_t columns, Reduction mode, Trace& out);

// How to combine values that are not decibels.
//
// Reduction converts through the power domain, which is right for levels and
// nonsense for anything else. Coherence is a ratio and phase is an angle;
// running either through 10^(x/10) produces a number with no meaning.
enum class LinearReduction {
    // Smallest value in the column.
    //
    // The default, and the right one for coherence: showing the best value in
    // a pixel column would hide exactly the dropouts a user is looking for.
    Min,
    // Arithmetic mean.
    Mean,
    // Circular mean, for angles in degrees.
    //
    // Averages unit vectors rather than numbers, so a column holding +179 deg
    // and -179 deg reads 180 rather than 0.
    Circular,
};

// Reduce non-decibel `values` onto the axis, one per pixel column.
//
// Mirrors reduce() exactly - same column walk, same DC skip, same sparse
// interpolation - and differs only in never treating a value as a level.
// Columns with no data read `fill` instead of negative infinity, since zero is
// a meaningful coherence and negative infinity is not.
void reduce_linear(std::span<const float> values, float bin_spacing_hz, const FrequencyAxis& axis,
                   std::size_t columns, LinearReduction mode, float fill, Trace& out);

// Helpers behind the reductions, declared here so the tests can reach them.
// Not part of the interface callers should build on.
namespace detail {

// Mean direction of angles in degrees. Opposed angles that cancel exactly have
// no mean direction and read 0 rather than NaN.
float circular_mean_degrees(std::span<const float> degrees) noexcept;

// Mean of decibel values, averaged as power. Negative infinity for no values.
float mean_db(std::span<const float> levels) noexcept;

}  // namespace detail

}  // namespace analyzer::plot
