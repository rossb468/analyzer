// Signal generation to file.
//
// Session-independent: rendering a signal needs no device, no stream and no
// analysis, so a client can write a test file without starting anything.

#include <cmath>
#include <filesystem>

#include "dsp/generator.hpp"
#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "model/wav.hpp"

using analyzer::ffi::guard_status;
using analyzer::ffi::set_status;
using analyzer::ffi::status_failure;
using analyzer::ffi::status_ok;

// `level_db` is in dBFS and capped at 0: a generator that can write above full
// scale can only write something that clips.
extern "C" uintptr_t analyzer_write_signal(const char* path, AnalyzerSignal signal, float hz,
                                           float end_hz, float level_db, float seconds,
                                           float sample_rate, AnalyzerSampleDepth depth,
                                           AnalyzerStatus* status) noexcept {
    if (path == nullptr) {
        set_status(status, status_failure("null path"));
        return 0;
    }
    return guard_status<std::uintptr_t>(status, 0, [&]() -> std::size_t {
        const auto file = analyzer::ffi::checked_utf8(path);
        if (!file) {
            set_status(status, status_failure("path is not valid UTF-8"));
            return 0;
        }

        const float amplitude = analyzer::ffi::amplitude_from_db(level_db);
        analyzer::dsp::Signal rendered;
        switch (signal) {
            case AnalyzerSignal_Sine: rendered = analyzer::dsp::Signal::sine(hz, amplitude); break;
            case AnalyzerSignal_WhiteNoise:
                rendered = analyzer::dsp::Signal::white_noise(amplitude);
                break;
            case AnalyzerSignal_PinkNoise:
                rendered = analyzer::dsp::Signal::pink_noise(amplitude);
                break;
            case AnalyzerSignal_Sweep:
                // One pass filling the file exactly. A repeating sweep would
                // overlap its own tail.
                rendered =
                    analyzer::dsp::Signal::sweep(hz, end_hz, seconds, amplitude, /*repeat=*/false);
                break;
            case AnalyzerSignal_Silence:
                set_status(status, status_failure("choose a signal to write"));
                return 0;
        }

        try {
            const std::size_t frames =
                analyzer::model::write_signal(std::filesystem::path(*file), rendered, sample_rate,
                                              seconds, analyzer::ffi::to_sample_depth(depth));
            set_status(status, status_ok());
            return frames;
        } catch (const std::exception& error) {
            set_status(status, status_failure("writing " + *file + ": " + error.what()));
            return 0;
        }
    });
}
