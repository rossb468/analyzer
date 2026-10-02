// Captured traces.
//
// See AnalyzerTraceStore for why the store is a handle of its own.

#include <algorithm>
#include <cmath>

#include "ffi/internal.hpp"
#include "ffi/session.hpp"
#include "model/measurement.hpp"

using analyzer::ffi::guard;

extern "C" AnalyzerTraceStore* analyzer_trace_store_create(void) noexcept {
    return guard<AnalyzerTraceStore*>(nullptr, [] { return new AnalyzerTraceStore(); });
}

extern "C" void analyzer_trace_store_destroy(AnalyzerTraceStore* store) noexcept {
    if (store == nullptr) {
        return;
    }
    guard([&] { delete store; });
}

extern "C" intptr_t analyzer_trace_store_capture(AnalyzerTraceStore* store,
                                                 AnalyzerSession* session,
                                                 const char* name) noexcept {
    if (store == nullptr || session == nullptr) {
        return -1;
    }
    return guard<std::intptr_t>(-1, [&]() -> std::intptr_t {
        analyzer::model::Measurement measurement =
            analyzer::ffi::live_measurement(*session, std::string());
        if (analyzer::model::is_empty(measurement.data)) {
            return -1;
        }

        // A null, empty or non-UTF-8 name asks for a generated one.
        std::string requested = analyzer::ffi::name_or(name, "");
        measurement.name = store->measurements.unique_name(requested.empty() ? "Trace" : requested);

        const analyzer::model::MeasurementId id = store->measurements.add(std::move(measurement));
        analyzer::ffi::CapturedTrace captured;
        captured.id = id;
        captured.visible = true;
        captured.colour = store->next_colour;
        ++store->next_colour;
        store->display.push_back(captured);
        return static_cast<std::intptr_t>(store->display.size() - 1);
    });
}

extern "C" uintptr_t analyzer_trace_store_count(const AnalyzerTraceStore* store) noexcept {
    if (store == nullptr) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&] { return store->display.size(); });
}

extern "C" bool analyzer_trace_store_info(const AnalyzerTraceStore* store, uintptr_t index,
                                          AnalyzerTraceInfo* out) noexcept {
    if (store == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        if (index >= store->display.size()) {
            return false;
        }
        const analyzer::ffi::CapturedTrace& display = store->display[index];
        const analyzer::model::Measurement* measurement = store->measurements.get(display.id);
        if (measurement == nullptr) {
            return false;
        }

        AnalyzerTraceInfo info{};
        info.visible = display.visible;
        info.colour = display.colour;
        info.points = analyzer::model::point_count(measurement->data);
        info.sample_rate = static_cast<float>(measurement->sample_rate);
        info.bin_spacing_hz =
            static_cast<float>(analyzer::model::bin_spacing_hz(measurement->data).value_or(0.0));
        analyzer::ffi::write_c_string(info.name, measurement->name);
        *out = info;
        return true;
    });
}

extern "C" bool analyzer_trace_store_set_visible(AnalyzerTraceStore* store, uintptr_t index,
                                                 bool visible) noexcept {
    if (store == nullptr) {
        return false;
    }
    return guard(false, [&] {
        if (index >= store->display.size()) {
            return false;
        }
        store->display[index].visible = visible;
        return true;
    });
}

extern "C" bool analyzer_trace_store_remove(AnalyzerTraceStore* store, uintptr_t index) noexcept {
    if (store == nullptr) {
        return false;
    }
    return guard(false, [&] {
        if (index >= store->display.size()) {
            return false;
        }
        const analyzer::ffi::CapturedTrace display = store->display[index];
        store->display.erase(store->display.begin() + static_cast<std::ptrdiff_t>(index));
        store->measurements.remove(display.id);
        return true;
    });
}

extern "C" void analyzer_trace_store_clear(AnalyzerTraceStore* store) noexcept {
    if (store == nullptr) {
        return;
    }
    guard([&] {
        store->display.clear();
        store->measurements.clear();
    });
}

// The session supplies only the geometry. A trace captured at one transform
// size draws correctly against a session running at another, which is the point
// of storing it at analysis resolution.
extern "C" uintptr_t analyzer_trace_store_copy(AnalyzerTraceStore* store, uintptr_t index,
                                               const AnalyzerSession* session, float* out,
                                               uintptr_t capacity) noexcept {
    if (store == nullptr || session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        if (index >= store->display.size()) {
            return 0;
        }
        const analyzer::ffi::CapturedTrace display = store->display[index];
        const analyzer::model::Measurement* measurement = store->measurements.get(display.id);
        if (measurement == nullptr) {
            return 0;
        }
        const auto levels = analyzer::model::magnitude_db(measurement->data);
        const auto spacing = analyzer::model::bin_spacing_hz(measurement->data);
        if (!levels || !spacing) {
            return 0;
        }

        store->bins.clear();
        store->bins.reserve(levels->size());
        for (const double db : *levels) {
            store->bins.push_back(static_cast<float>(db));
        }

        const std::size_t columns = std::min(session->columns, static_cast<std::size_t>(capacity));
        analyzer::plot::reduce(store->bins, static_cast<float>(*spacing), session->frequency,
                               columns, session->reduction, store->reduced);

        const std::size_t written =
            std::min(store->reduced.points.size(), static_cast<std::size_t>(capacity));
        std::copy_n(store->reduced.points.begin(), written, out);
        return written;
    });
}
