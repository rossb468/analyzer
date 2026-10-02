// Spectrogram.
//
// One column of data per analysis frame, and nothing more. The renderer keeps a
// ring-buffer texture on the GPU, writes this column into it, and scrolls by
// advancing a texture coordinate.
//
// The core must never composite the image. At a Retina drawable of roughly
// 2800x1600 that is 18 MB per frame, over 2 GB/s of CPU writes at 120 fps -
// which would make the CPU the frame rate limit and is a restatement of exactly
// the problem this project exists to avoid.

#include <algorithm>
#include <cmath>
#include <limits>

#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::guard;

// The spectrogram runs frequency up the drawable rather than across it, so it
// needs tick positions along a different length from the one the trace plot
// declared. This builds a temporary axis over the session's current frequency
// range and leaves the session's own geometry untouched, so asking does not
// disturb what the trace plot is drawing.
extern "C" uintptr_t analyzer_frequency_ticks_for(const AnalyzerSession* session, float length,
                                                  AnalyzerTick* out, uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0 || !std::isfinite(length) ||
        length <= 0.0f) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&] {
        const analyzer::plot::FrequencyAxis axis(session->frequency.min_hz(),
                                                 session->frequency.max_hz(), length);
        const std::vector<analyzer::plot::Tick> ticks = axis.ticks();
        return analyzer::ffi::write_ticks(ticks, out, capacity);
    });
}

// Values are decibels, not colours: mapping level to colour is the renderer's
// job and differs per platform. Returns the number of rows written, which is
// zero until something has been analysed.
extern "C" uintptr_t analyzer_session_copy_spectrogram_column(AnalyzerSession* session, float* out,
                                                              uintptr_t rows) noexcept {
    if (session == nullptr || out == nullptr || rows == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        // The spectrogram's frequency axis runs up the drawable, so it is sized
        // by row count rather than by the plot's column count.
        const float height = static_cast<float>(rows);
        if (std::fabs(session->spectrogram_axis.width() - height) >
                std::numeric_limits<float>::epsilon() ||
            session->spectrogram_axis.min_hz() != session->frequency.min_hz() ||
            session->spectrogram_axis.max_hz() != session->frequency.max_hz()) {
            session->spectrogram_axis = analyzer::plot::FrequencyAxis(
                session->frequency.min_hz(), session->frequency.max_hz(), height);
        }

        const analyzer::engine::SpectrumFrame& frame = session->engine.latest();
        if (frame.bins.empty()) {
            return 0;
        }
        session->bins.assign(frame.bins.begin(), frame.bins.end());
        const float spacing = frame.bin_spacing_hz;

        analyzer::plot::reduce(session->bins, spacing, session->spectrogram_axis, rows,
                               session->reduction, session->spectrogram_column);

        const std::size_t written =
            std::min(session->spectrogram_column.points.size(), static_cast<std::size_t>(rows));
        std::copy_n(session->spectrogram_column.points.begin(), written, out);
        return written;
    });
}
