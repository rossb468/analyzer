// The transfer function: its curves, its delay, and the stimulus that drives it.

#include <algorithm>
#include <cmath>

#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::guard;

// Each curve is reduced in the domain it actually lives in. Magnitude is
// decibels and goes through the session's chosen reduction. Coherence takes the
// worst value in a column, because showing the best would hide the dropouts a
// user is looking for. Phase is averaged as a direction, so a column straddling
// the wrap reads 180 rather than 0.
extern "C" uintptr_t analyzer_session_copy_transfer(AnalyzerSession* session, AnalyzerCurve curve,
                                                    float* out, uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        const std::size_t columns = std::min(session->columns, static_cast<std::size_t>(capacity));

        float spacing = 0.0f;
        {
            const analyzer::engine::SpectrumFrame& frame = session->engine.latest();
            if (frame.transfer_frames == 0) {
                return 0;
            }
            const std::vector<float>& source =
                curve == AnalyzerCurve_Phase       ? frame.transfer_phase_degrees
                : curve == AnalyzerCurve_Coherence ? frame.transfer_coherence
                                                   : frame.transfer_magnitude_db;
            session->bins.assign(source.begin(), source.end());
            spacing = frame.bin_spacing_hz;
        }

        switch (curve) {
            case AnalyzerCurve_Phase:
                analyzer::plot::reduce_linear(session->bins, spacing, session->frequency, columns,
                                              analyzer::plot::LinearReduction::Circular, 0.0f,
                                              session->transfer_trace);
                break;
            case AnalyzerCurve_Coherence:
                analyzer::plot::reduce_linear(session->bins, spacing, session->frequency, columns,
                                              analyzer::plot::LinearReduction::Min, 0.0f,
                                              session->transfer_trace);
                break;
            case AnalyzerCurve_Magnitude:
                analyzer::plot::reduce(session->bins, spacing, session->frequency, columns,
                                       session->reduction, session->transfer_trace);
                break;
        }

        const std::size_t written =
            std::min(session->transfer_trace.points.size(), static_cast<std::size_t>(capacity));
        std::copy_n(session->transfer_trace.points.begin(), written, out);
        return written;
    });
}

extern "C" bool analyzer_session_transfer_info(AnalyzerSession* session,
                                               AnalyzerTransferInfo* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        const std::uint32_t delay = session->engine.reference_delay();
        const float rate = std::fmax(session->engine.latest().sample_rate, 1.0f);
        const float seconds = static_cast<float>(delay) / rate;
        AnalyzerTransferInfo info{};
        info.frames = session->engine.latest().transfer_frames;
        info.delay_frames = delay;
        info.delay_ms = seconds * 1000.0f;
        info.delay_metres = seconds * analyzer::ffi::kSpeedOfSound;
        info.estimating = session->engine.delay_estimate_pending();
        *out = info;
        return true;
    });
}

extern "C" bool analyzer_session_estimate_delay(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        session->engine.estimate_reference_delay();
        return true;
    });
}

extern "C" bool analyzer_session_set_delay(AnalyzerSession* session, uint32_t frames) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        session->engine.set_reference_delay(frames);
        return true;
    });
}

extern "C" bool analyzer_session_set_signal(AnalyzerSession* session, AnalyzerSignal stimulus,
                                            float level_db, float hz) noexcept {
    if (session == nullptr || !std::isfinite(level_db) || !std::isfinite(hz)) {
        return false;
    }
    return guard(false, [&] {
        session->signal->set(stimulus, level_db, hz);
        return true;
    });
}
