// Axis queries.
//
// These exist so the UI never reimplements the mapping. Cursor readout,
// hit-testing and the drawn geometry must agree exactly, and duplicating the
// maths in Swift is how they quietly stop agreeing.

#include <algorithm>
#include <cmath>
#include <limits>

#include "ffi/internal.hpp"
#include "ffi/session.hpp"

namespace analyzer::ffi {

std::size_t write_ticks(std::span<const plot::Tick> ticks, AnalyzerTick* out,
                        std::size_t capacity) noexcept {
    const std::size_t written = std::min(ticks.size(), capacity);
    for (std::size_t i = 0; i < written; ++i) {
        out[i].value = ticks[i].value;
        out[i].position = ticks[i].position;
        out[i].major = ticks[i].major;
    }
    return written;
}

}  // namespace analyzer::ffi

using analyzer::ffi::guard;

namespace {

constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

}  // namespace

extern "C" float analyzer_freq_to_x(const AnalyzerSession* session, float hz) noexcept {
    if (session == nullptr) {
        return kNaN;
    }
    return guard(kNaN, [&] { return session->frequency.freq_to_x(hz); });
}

extern "C" float analyzer_x_to_freq(const AnalyzerSession* session, float x) noexcept {
    if (session == nullptr) {
        return kNaN;
    }
    return guard(kNaN, [&] { return session->frequency.x_to_freq(x); });
}

extern "C" float analyzer_db_to_y(const AnalyzerSession* session, float db) noexcept {
    if (session == nullptr) {
        return kNaN;
    }
    return guard(kNaN, [&] { return session->level.db_to_y(db); });
}

extern "C" float analyzer_y_to_db(const AnalyzerSession* session, float y) noexcept {
    if (session == nullptr) {
        return kNaN;
    }
    return guard(kNaN, [&] { return session->level.y_to_db(y); });
}

extern "C" float analyzer_phase_to_y(const AnalyzerSession* session, float degrees) noexcept {
    if (session == nullptr) {
        return kNaN;
    }
    return guard(kNaN, [&] { return session->phase.db_to_y(degrees); });
}

extern "C" float analyzer_coherence_to_y(const AnalyzerSession* session, float value) noexcept {
    if (session == nullptr) {
        return kNaN;
    }
    return guard(kNaN, [&] { return session->coherence.db_to_y(value); });
}

extern "C" uintptr_t analyzer_frequency_ticks(const AnalyzerSession* session, AnalyzerTick* out,
                                              uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&] {
        const std::vector<analyzer::plot::Tick> ticks = session->frequency.ticks();
        return analyzer::ffi::write_ticks(ticks, out, capacity);
    });
}

extern "C" uintptr_t analyzer_level_ticks(const AnalyzerSession* session, float step_db,
                                          AnalyzerTick* out, uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0 || !std::isfinite(step_db)) {
        return 0;
    }
    if (step_db <= 0.0f) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&] {
        const std::vector<analyzer::plot::Tick> ticks = session->level.ticks(step_db);
        return analyzer::ffi::write_ticks(ticks, out, capacity);
    });
}
