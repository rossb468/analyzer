// Writing the current spectrum to disk.

#include <string>

#include "model/export.hpp"
#include "model/format.hpp"

#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::guard_status;
using analyzer::ffi::set_status;
using analyzer::ffi::status_failure;
using analyzer::ffi::status_ok;

namespace {

// Bytes of a measurement file as the text view the writer takes.
std::string_view as_text(const std::vector<std::byte>& bytes) {
    return std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

}  // namespace

// Writes a magnitude-only measurement, because that is what an RTA produces.
//
// Whether the levels are dB SPL or dBFS is recorded rather than assumed:
// `spl_offset_db` is stored when non-zero and omitted otherwise, so a reader can
// tell a calibrated measurement from an uncalibrated one.
extern "C" bool analyzer_session_save_measurement(AnalyzerSession* session, const char* path,
                                                  const char* name, float spl_offset_db,
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

        analyzer::model::Measurement measurement =
            analyzer::ffi::live_measurement(*session, analyzer::ffi::name_or(name, "Measurement"));
        if (analyzer::model::is_empty(measurement.data)) {
            set_status(status, status_failure("nothing captured yet"));
            return false;
        }
        // Zero means uncalibrated here, which stays unset - "not measured" and
        // "measured as needing no correction" are different facts.
        if (spl_offset_db != 0.0f) {
            measurement.references.spl_offset_db = static_cast<double>(spl_offset_db);
        }

        if (const auto error = analyzer::ffi::write_file(
                *file, as_text(analyzer::model::write_measurement(measurement)))) {
            set_status(status, status_failure("writing " + *file + ": " + *error));
            return false;
        }
        set_status(status, status_ok());
        return true;
    });
}

extern "C" bool analyzer_session_export_text(AnalyzerSession* session, const char* path,
                                             const char* name, float spl_offset_db,
                                             AnalyzerStatus* status) noexcept {
    if (session == nullptr || path == nullptr) {
        set_status(status, status_failure("null session or path"));
        return false;
    }
    return guard_status(status, false, [&] {
        const auto file = analyzer::ffi::checked_utf8(path);
        if (!file) {
            // Reported by return value alone, as it always has been.
            return false;
        }

        analyzer::model::Measurement measurement =
            analyzer::ffi::live_measurement(*session, analyzer::ffi::name_or(name, "Measurement"));
        if (spl_offset_db != 0.0f) {
            measurement.references.spl_offset_db = static_cast<double>(spl_offset_db);
        }

        if (const auto error =
                analyzer::ffi::write_file(*file, analyzer::model::to_rew_text(measurement))) {
            set_status(status, status_failure("writing " + *file + ": " + *error));
            return false;
        }
        set_status(status, status_ok());
        return true;
    });
}
