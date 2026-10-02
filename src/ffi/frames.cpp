// Plot geometry and the frame data that goes out through it: traces, the
// average, frame metadata and the distortion readout.

#include <algorithm>
#include <cmath>

#include "base/numeric.hpp"
#include "dsp/distortion.hpp"
#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::guard;
using analyzer::ffi::Which;

extern "C" bool analyzer_session_set_plot(AnalyzerSession* session, float width_px, float height_px,
                                          float min_hz, float max_hz, float min_db, float max_db,
                                          AnalyzerReduction reduction) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        // Reject degenerate geometry here rather than letting the axis
        // constructors abort across the boundary. Finiteness is checked
        // explicitly: a NaN arriving from a UI layout calculation would slip
        // past a bare comparison and poison every subsequent transform.
        const bool usable = std::isfinite(width_px) && std::isfinite(height_px) &&
                            std::isfinite(min_hz) && std::isfinite(max_hz) &&
                            std::isfinite(min_db) && std::isfinite(max_db) && width_px > 0.0f &&
                            height_px > 0.0f && min_hz > 0.0f && max_hz > min_hz && max_db > min_db;
        if (!usable) {
            return false;
        }
        session->frequency = analyzer::plot::FrequencyAxis(min_hz, max_hz, width_px);
        session->level = analyzer::plot::LevelAxis(min_db, max_db, height_px);
        // Fixed ranges over the same pixels. Phase spans one full turn and
        // coherence spans its whole domain, so neither ever needs rescaling and
        // a UI cannot accidentally clip either.
        session->phase = analyzer::plot::LevelAxis(-180.0f, 180.0f, height_px);
        session->coherence = analyzer::plot::LevelAxis(0.0f, 1.0f, height_px);
        session->column_hz.clear();
        session->reduction = analyzer::ffi::to_reduction(reduction);
        session->columns =
            analyzer::saturating_cast<std::size_t>(std::fmax(std::round(width_px), 1.0f));
        return true;
    });
}

extern "C" bool analyzer_session_has_new_frame(const AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] { return session->engine.has_new_frame(); });
}

extern "C" uintptr_t analyzer_session_copy_trace(AnalyzerSession* session, float* out,
                                                 uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0,
                                 [&] { return session->copy_reduced(out, capacity, Which::Live); });
}

extern "C" uintptr_t analyzer_session_copy_average(AnalyzerSession* session, float* out,
                                                   uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(
        0, [&] { return session->copy_reduced(out, capacity, Which::Average); });
}

extern "C" bool analyzer_session_reset_average(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        session->engine.reset_average();
        return true;
    });
}

extern "C" bool analyzer_session_frame_info(AnalyzerSession* session,
                                            AnalyzerFrameInfo* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        const analyzer::engine::SpectrumFrame& frame = session->engine.latest();
        out->sequence = frame.sequence;
        out->overruns = frame.overruns;
        out->frames_averaged = frame.frames_averaged;
        out->average_frames = frame.average_frames;
        out->sample_rate = frame.sample_rate;
        out->bin_spacing_hz = frame.bin_spacing_hz;
        return true;
    });
}

extern "C" bool analyzer_session_distortion(AnalyzerSession* session, float fundamental_hz,
                                            AnalyzerDistortion* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        // The frame carries decibels; the analysis needs linear power. The dB
        // came from power in the first place, so this inverts exactly the
        // conversion that produced it. Round-tripping through f32 dB costs far
        // less precision than the measurement itself has.
        const analyzer::engine::SpectrumFrame& frame = session->engine.latest();
        session->power.clear();
        session->power.reserve(frame.bins.size());
        for (const float db : frame.bins) {
            session->power.push_back(std::pow(10.0f, db / 10.0f) / 2.0f);
        }
        const float spacing = frame.bin_spacing_hz;

        std::optional<float> hint;
        if (fundamental_hz > 0.0f) {
            hint = fundamental_hz;
        }
        const std::optional<analyzer::dsp::Distortion> result = analyzer::dsp::analyse_distortion(
            session->power, spacing, hint, analyzer::dsp::DistortionConfig{});
        if (!result) {
            return false;
        }

        // A fundamental buried in the floor makes every derived figure noise.
        constexpr float kMinimumHeadroomDb = 20.0f;
        if (result->fundamental_db < result->noise_floor_db + kMinimumHeadroomDb) {
            return false;
        }

        AnalyzerDistortion value{};
        value.fundamental_hz = result->fundamental_hz;
        value.fundamental_db = result->fundamental_db;
        value.thd_percent = result->thd_percent;
        value.thd_db = result->thd_db;
        value.thd_n_percent = result->thd_n_percent;
        value.noise_floor_db = result->noise_floor_db;
        value.harmonic_count = 0;
        value.orders_above_nyquist = result->orders_above_nyquist;
        for (const analyzer::dsp::Harmonic& harmonic : result->harmonics) {
            if (value.harmonic_count == ANALYZER_MAX_HARMONICS) {
                break;
            }
            const std::size_t index = value.harmonic_count;
            value.harmonic_hz[index] = harmonic.hz;
            value.harmonic_percent[index] = harmonic.percent;
            value.harmonic_relative_db[index] = harmonic.relative_db;
            ++value.harmonic_count;
        }

        *out = value;
        return true;
    });
}

extern "C" uintptr_t analyzer_session_device_name(const AnalyzerSession* session, char* out,
                                                  uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&] {
        const std::string& name = session->device_name;
        const std::size_t end =
            analyzer::ffi::utf8_floor(name, std::min<std::size_t>(name.size(), capacity - 1));
        std::copy_n(name.begin(), end, out);
        out[end] = '\0';
        return end;
    });
}
