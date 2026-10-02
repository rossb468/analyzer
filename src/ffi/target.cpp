// Target curves: the response a correction is aiming at.

#include <algorithm>
#include <utility>

#include "cal/curve.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::guard;
using analyzer::ffi::guard_status;
using analyzer::ffi::set_status;
using analyzer::ffi::status_failure;
using analyzer::ffi::status_ok;

extern "C" AnalyzerTarget analyzer_target_default(void) noexcept {
    return analyzer::ffi::default_target();
}

extern "C" bool analyzer_session_target(const AnalyzerSession* session,
                                        AnalyzerTarget* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        *out = session->target_description();
        return true;
    });
}

extern "C" bool analyzer_session_set_target(AnalyzerSession* session,
                                            const AnalyzerTarget* target) noexcept {
    if (session == nullptr || target == nullptr) {
        return false;
    }
    return guard(false, [&] {
        session->apply_target(*target);
        return true;
    });
}

extern "C" bool analyzer_session_load_target(AnalyzerSession* session, const char* path,
                                             AnalyzerStatus* status) noexcept {
    if (session == nullptr || path == nullptr) {
        set_status(status, status_failure("null session or path"));
        return false;
    }
    return guard_status(status, false, [&] {
        const auto file = analyzer::ffi::checked_utf8(path);
        if (!file) {
            set_status(status, status_failure("path is not valid UTF-8"));
            return false;
        }

        const analyzer::ffi::FileRead read = analyzer::ffi::read_text_file(*file);
        if (read.error) {
            set_status(status, status_failure("reading " + *file + ": " + *read.error));
            return false;
        }

        // The calibration parser already handles the frequency/level text these
        // files ship as, including the comment and header conventions.
        const analyzer::cal::ResponseCurve curve = analyzer::cal::ResponseCurve::parse(read.text);
        if (curve.points().empty()) {
            set_status(status, status_failure("no frequency and level pairs found in that file"));
            return false;
        }

        std::vector<std::pair<float, float>> points;
        points.reserve(curve.points().size());
        for (const analyzer::cal::CurvePoint& point : curve.points()) {
            points.emplace_back(point.hz, point.db);
        }
        session->target.set_shape(analyzer::dsp::TargetShape::custom(std::move(points)));
        set_status(status, status_ok());
        return true;
    });
}

// A target is relative, so without this it floats somewhere unrelated to the
// measurement and every error computed against it is dominated by a constant.
extern "C" bool analyzer_session_align_target(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] { return session->align_target(); });
}

extern "C" uintptr_t analyzer_session_copy_target(AnalyzerSession* session, float* out,
                                                  uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        const std::size_t columns = std::min(session->columns, static_cast<std::size_t>(capacity));
        session->columns_hz(columns);

        session->target_trace.points.clear();
        session->target_trace.points.reserve(columns);
        const std::size_t count = std::min(columns, session->column_hz.size());
        for (std::size_t i = 0; i < count; ++i) {
            session->target_trace.points.push_back(session->target.db_at(session->column_hz[i]));
        }

        const std::size_t written =
            std::min(session->target_trace.points.size(), static_cast<std::size_t>(capacity));
        std::copy_n(session->target_trace.points.begin(), written, out);
        return written;
    });
}
