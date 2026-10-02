// The equaliser entry points.
//
// Both equalisers are kept across a mode switch, so moving to the parametric
// and back does not lose the graphic's fader positions. Every change republishes
// the active one's coefficients to the audio thread.

#include <algorithm>
#include <cmath>

#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::guard;

namespace {

bool is_finite(const analyzer::dsp::FilterBand& band) {
    return std::isfinite(band.hz) && std::isfinite(band.gain_db) && std::isfinite(band.q);
}

}  // namespace

extern "C" bool analyzer_session_set_eq_mode(AnalyzerSession* session,
                                             AnalyzerEqMode mode) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        session->eq_mode = mode;
        session->publish_eq();
        return true;
    });
}

extern "C" uintptr_t analyzer_session_eq_band_count(const AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&] {
        const analyzer::dsp::Equaliser* eq = session->equaliser();
        return eq == nullptr ? 0 : eq->bands().size();
    });
}

extern "C" bool analyzer_session_eq_get_band(const AnalyzerSession* session, uintptr_t index,
                                             AnalyzerBand* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        const analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr || index >= eq->bands().size()) {
            return false;
        }
        *out = analyzer::ffi::to_c(eq->bands()[index]);
        return true;
    });
}

extern "C" bool analyzer_session_eq_set_band(AnalyzerSession* session, uintptr_t index,
                                             const AnalyzerBand* band) noexcept {
    if (session == nullptr || band == nullptr) {
        return false;
    }
    return guard(false, [&] {
        const analyzer::dsp::FilterBand wanted = analyzer::ffi::to_filter_band(*band);
        if (!is_finite(wanted)) {
            return false;
        }
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr || index >= eq->bands().size()) {
            return false;
        }
        eq->set_band(index, wanted);
        session->publish_eq();
        return true;
    });
}

extern "C" bool analyzer_session_eq_set_gain(AnalyzerSession* session, uintptr_t index,
                                             float gain_db) noexcept {
    if (session == nullptr || !std::isfinite(gain_db)) {
        return false;
    }
    return guard(false, [&] {
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr || index >= eq->bands().size()) {
            return false;
        }
        eq->set_gain_db(index, gain_db);
        session->publish_eq();
        return true;
    });
}

extern "C" intptr_t analyzer_session_eq_add_band(AnalyzerSession* session,
                                                 const AnalyzerBand* band) noexcept {
    if (session == nullptr || band == nullptr) {
        return -1;
    }
    return guard<std::intptr_t>(-1, [&]() -> std::intptr_t {
        const analyzer::dsp::FilterBand wanted = analyzer::ffi::to_filter_band(*band);
        if (!is_finite(wanted)) {
            return -1;
        }
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            return -1;
        }
        // The audio thread's coefficient array is fixed, so a band beyond it
        // would silently not be heard. Refusing is the honest answer.
        if (eq->bands().size() >= analyzer::ffi::kMaxEqBands) {
            return -1;
        }
        const std::size_t index = eq->push_band(wanted);
        session->publish_eq();
        return static_cast<std::intptr_t>(index);
    });
}

extern "C" bool analyzer_session_eq_remove_band(AnalyzerSession* session,
                                                uintptr_t index) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr || index >= eq->bands().size()) {
            return false;
        }
        eq->remove_band(index);
        session->publish_eq();
        return true;
    });
}

extern "C" bool analyzer_session_eq_flatten(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            return false;
        }
        eq->flatten();
        session->publish_eq();
        return true;
    });
}

extern "C" bool analyzer_session_eq_trim(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] {
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            return false;
        }
        eq->trim_to_unity();
        session->publish_eq();
        return true;
    });
}

extern "C" bool analyzer_session_eq_set_preamp(AnalyzerSession* session, float db) noexcept {
    if (session == nullptr || !std::isfinite(db)) {
        return false;
    }
    return guard(false, [&] {
        analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            return false;
        }
        eq->set_preamp_db(db);
        session->publish_eq();
        return true;
    });
}

extern "C" bool analyzer_session_eq_info(const AnalyzerSession* session,
                                         AnalyzerEqInfo* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        AnalyzerEqInfo info{};
        if (const analyzer::dsp::Equaliser* eq = session->equaliser()) {
            info.band_count = eq->bands().size();
            info.peak_gain_db = eq->peak_gain_db();
            info.preamp_db = eq->preamp_db();
            info.active = true;
        }
        *out = info;
        return true;
    });
}

// This is arithmetic on the coefficients, not a measurement: it works whether
// or not audio is running, which is what makes designing a correction against a
// saved measurement possible.
extern "C" uintptr_t analyzer_session_copy_eq_curve(AnalyzerSession* session, intptr_t band,
                                                    float* out, uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        const analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            return 0;
        }
        const std::size_t columns = std::min(session->columns, static_cast<std::size_t>(capacity));
        session->columns_hz(columns);

        session->eq_trace.points.clear();
        session->eq_trace.points.reserve(columns);
        const std::size_t count = std::min(columns, session->column_hz.size());
        for (std::size_t i = 0; i < count; ++i) {
            const float hz = session->column_hz[i];
            session->eq_trace.points.push_back(
                band < 0 ? eq->magnitude_db_at(hz)
                         : eq->band_magnitude_db_at(static_cast<std::size_t>(band), hz));
        }

        const std::size_t written =
            std::min(session->eq_trace.points.size(), static_cast<std::size_t>(capacity));
        std::copy_n(session->eq_trace.points.begin(), written, out);
        return written;
    });
}

// The point of an equaliser in a measurement tool: what the room would look
// like after the correction, drawn beside what it looks like now. Adding
// decibels is exact here because the equaliser's curve is known analytically
// rather than measured.
extern "C" uintptr_t analyzer_session_copy_corrected(AnalyzerSession* session, float* out,
                                                     uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        const analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            return 0;
        }
        const std::size_t written =
            session->copy_reduced(out, capacity, analyzer::ffi::Which::Live);
        if (written == 0) {
            return 0;
        }
        session->columns_hz(written);

        const std::size_t count = std::min(written, session->column_hz.size());
        for (std::size_t i = 0; i < count; ++i) {
            out[i] += eq->magnitude_db_at(session->column_hz[i]);
        }
        return written;
    });
}
