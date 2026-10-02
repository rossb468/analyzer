// Automatic equalisation and filter export.

#include "dsp/optimise.hpp"
#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"
#include "model/filter_export.hpp"

using analyzer::ffi::guard_status;
using analyzer::ffi::set_status;
using analyzer::ffi::status_failure;
using analyzer::ffi::status_ok;

extern "C" AnalyzerOptimiserConfig analyzer_optimiser_config_default(void) noexcept {
    const analyzer::dsp::OptimiserConfig defaults;
    AnalyzerOptimiserConfig config{};
    config.max_filters = static_cast<std::uint32_t>(defaults.max_filters);
    config.from_hz = defaults.from_hz;
    config.to_hz = defaults.to_hz;
    config.max_boost_db = defaults.max_boost_db;
    config.max_cut_db = defaults.max_cut_db;
    config.min_q = defaults.min_q;
    config.max_q = defaults.max_q;
    config.threshold_db = defaults.threshold_db;
    return config;
}

// The result replaces the parametric equaliser's bands and selects it, so the
// fit is immediately drawn and heard. Replacing rather than appending is
// deliberate: running the fit twice should give the same answer as running it
// once, and appending would instead correct the correction.
extern "C" bool analyzer_session_optimise(AnalyzerSession* session,
                                          const AnalyzerOptimiserConfig* config,
                                          AnalyzerOptimisation* out,
                                          AnalyzerStatus* status) noexcept {
    if (session == nullptr || config == nullptr) {
        set_status(status, status_failure("null session or configuration"));
        return false;
    }
    return guard_status(status, false, [&] {
        analyzer::dsp::OptimiserConfig settings;
        settings.max_filters = config->max_filters;
        settings.from_hz = config->from_hz;
        settings.to_hz = config->to_hz;
        settings.max_boost_db = config->max_boost_db;
        settings.max_cut_db = config->max_cut_db;
        settings.min_q = config->min_q;
        settings.max_q = config->max_q;
        settings.threshold_db = config->threshold_db;
        // Only the session knows the rate.
        settings.sample_rate = session->parametric.sample_rate();

        const auto measured = session->measured_at_columns();
        if (!measured) {
            set_status(status, status_failure("nothing has been captured yet to correct"));
            return false;
        }

        // A target sitting at the wrong absolute level would make every error
        // the fit sees a constant offset, and it would spend its filters on that
        // rather than on the room.
        session->target.align_to(measured->frequencies, measured->levels,
                                 analyzer::dsp::kAlignFromHz, analyzer::dsp::kAlignToHz);

        analyzer::dsp::Optimisation result = analyzer::dsp::optimise(
            measured->frequencies, measured->levels, session->target, settings);

        const std::size_t band_count = result.bands.size();
        session->parametric.set_bands(std::move(result.bands));
        session->eq_mode = AnalyzerEqMode_Parametric;
        session->publish_eq();

        if (out != nullptr) {
            AnalyzerOptimisation optimisation{};
            optimisation.band_count = static_cast<std::uint32_t>(band_count);
            optimisation.initial_error_db = result.initial_error_db;
            optimisation.final_error_db = result.final_error_db;
            *out = optimisation;
        }
        set_status(status, status_ok());
        return true;
    });
}

// Fails when no equaliser is active, rather than writing an empty file that
// looks like a successful export of nothing.
extern "C" bool analyzer_session_export_filters(const AnalyzerSession* session,
                                                AnalyzerFilterFormat format, const char* path,
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

        const analyzer::dsp::Equaliser* eq = session->equaliser();
        if (eq == nullptr) {
            set_status(status, status_failure("no equaliser is active"));
            return false;
        }

        const std::string text =
            analyzer::model::to_text(analyzer::ffi::to_filter_format(format), eq->bands(),
                                     eq->preamp_db(), eq->sample_rate());
        if (const auto error = analyzer::ffi::write_file(*file, text)) {
            set_status(status, status_failure("writing " + *file + ": " + *error));
            return false;
        }
        set_status(status, status_ok());
        return true;
    });
}
