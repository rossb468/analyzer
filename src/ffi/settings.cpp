// Settings.
//
// Preferences live in the core rather than in the platform's own defaults
// store, so their validation and their file format are written once. The
// platform supplies only the location - which directory a preferences file
// belongs in is genuinely a platform question, and the one part of this that
// Windows and Linux will answer differently.

#include <filesystem>
#include <system_error>

#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "model/settings.hpp"

using analyzer::ffi::guard;
using analyzer::ffi::guard_status;
using analyzer::ffi::set_status;
using analyzer::ffi::status_failure;
using analyzer::ffi::status_ok;

extern "C" AnalyzerSettings analyzer_settings_default(void) noexcept {
    return guard(AnalyzerSettings{},
                 [] { return analyzer::ffi::to_c(analyzer::model::Settings{}); });
}

// A file that does not exist is not a failure: it is the first launch, and the
// defaults are written through with a success status. Anything else - an
// unreadable directory, a permissions problem - is reported, because silently
// starting from defaults there would look identical to the settings having been
// lost.
extern "C" bool analyzer_settings_load(const char* path, AnalyzerSettings* out,
                                       AnalyzerStatus* status) noexcept {
    if (path == nullptr || out == nullptr) {
        set_status(status, status_failure("null path or destination"));
        return false;
    }
    return guard_status(status, false, [&] {
        const auto file = analyzer::ffi::checked_utf8(path);
        if (!file) {
            set_status(status, status_failure("path is not valid UTF-8"));
            return false;
        }

        const analyzer::ffi::FileRead read = analyzer::ffi::read_text_file(*file);
        if (!read.error) {
            *out = analyzer::ffi::to_c(analyzer::model::Settings::from_text(read.text));
            set_status(status, status_ok());
            return true;
        }
        *out = analyzer_settings_default();
        if (read.not_found) {
            set_status(status, status_ok());
            return true;
        }
        set_status(status, status_failure("reading " + *file + ": " + *read.error));
        return false;
    });
}

// Creates the containing directory if needed.
extern "C" bool analyzer_settings_save(const char* path, const AnalyzerSettings* settings,
                                       AnalyzerStatus* status) noexcept {
    if (path == nullptr || settings == nullptr) {
        set_status(status, status_failure("null path or settings"));
        return false;
    }
    return guard_status(status, false, [&] {
        const auto file = analyzer::ffi::checked_utf8(path);
        if (!file) {
            set_status(status, status_failure("path is not valid UTF-8"));
            return false;
        }
        const analyzer::model::Settings validated = analyzer::ffi::to_settings(*settings);

        // A bare file name has an empty parent, which is the current directory
        // and needs nothing creating.
        const std::filesystem::path parent = std::filesystem::path(*file).parent_path();
        if (!parent.empty()) {
            std::error_code error;
            std::filesystem::create_directories(parent, error);
            if (error) {
                set_status(status, status_failure("creating " + parent.string() + ": " +
                                                  analyzer::ffi::os_error_text(error.value())));
                return false;
            }
        }

        if (const auto error = analyzer::ffi::write_file(*file, validated.to_text())) {
            set_status(status, status_failure("writing " + *file + ": " + *error));
            return false;
        }
        set_status(status, status_ok());
        return true;
    });
}
