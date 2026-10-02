// Swept measurement.
//
// A sweep is played, the response recorded, and the two deconvolved into an
// impulse response. Everything the measurement reports - the gated frequency
// response, the decay times, the arrival - falls out of that one impulse
// response rather than being measured separately.

#include <algorithm>
#include <cmath>
#include <limits>

#include "base/numeric.hpp"
#include "dsp/deconv.hpp"
#include "dsp/generator.hpp"
#include "dsp/ir.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"

using analyzer::ffi::clamp_float;
using analyzer::ffi::guard;
using analyzer::ffi::guard_status;
using analyzer::ffi::set_status;
using analyzer::ffi::status_failure;
using analyzer::ffi::status_ok;

namespace {

// Index of the largest magnitude in `samples`, the last of them when several are
// equal, and zero for none.
std::size_t peak_index(std::span<const float> samples) {
    if (samples.empty()) {
        return 0;
    }
    const auto less = [](float a, float b) {
        return analyzer::ffi::total_less(std::fabs(a), std::fabs(b));
    };
    // Searching backwards makes the first maximum found the last in time.
    const auto found = std::max_element(samples.rbegin(), samples.rend(), less);
    return samples.size() - 1 - static_cast<std::size_t>(found - samples.rbegin());
}

}  // namespace

extern "C" AnalyzerMeasureConfig analyzer_measure_config_default(void) noexcept {
    AnalyzerMeasureConfig config{};
    config.start_hz = 20.0f;
    config.end_hz = 20'000.0f;
    config.seconds = 2.0f;
    // Well below full scale. A measurement sweep that defaults to loud is a
    // measurement sweep that damages something.
    config.level_db = -12.0f;
    config.tail_seconds = 1.0f;
    config.gate_ms = 5.0f;
    config.fft_size = 16'384;
    return config;
}

// Arms the recording before the sweep so nothing is missed, then sets the
// generator. Fails when the session has no output stream to play through, which
// is the common case on a laptop whose input and output are separate devices.
extern "C" bool analyzer_session_start_measurement(AnalyzerSession* session,
                                                   const AnalyzerMeasureConfig* config,
                                                   AnalyzerStatus* status) noexcept {
    if (session == nullptr || config == nullptr) {
        set_status(status, status_failure("null session or configuration"));
        return false;
    }
    return guard_status(status, false, [&] {
        const AnalyzerMeasureConfig wanted = *config;

        const float rate = session->engine.config().spectrum.sample_rate;
        const float seconds = clamp_float(wanted.seconds, 0.1f, 30.0f);
        const float tail = clamp_float(wanted.tail_seconds, 0.0f, 10.0f);
        const float start_hz = clamp_float(wanted.start_hz, 1.0f, rate / 2.0f);
        const float end_hz = clamp_float(wanted.end_hz, start_hz * 1.01f, rate / 2.0f);
        const auto frames = analyzer::saturating_cast<std::size_t>(
            std::fmax(std::round((seconds + tail) * rate), 1.0f));

        // The deconvolution transform is sized from the recording, and it has to
        // hold twice it. Refusing here is better than producing a truncated
        // impulse response that looks like a short room.
        if (frames > (std::size_t{1} << 22)) {
            set_status(status, status_failure("that sweep is longer than the measurement buffer"));
            return false;
        }

        session->measured.reset();
        // Armed before the sweep starts, so the recording cannot begin partway
        // through it.
        session->engine.begin_recording(frames);
        session->signal->set_sweep(start_hz, end_hz, seconds, wanted.level_db);

        analyzer::ffi::MeasurementRun run;
        run.start_hz = start_hz;
        run.end_hz = end_hz;
        run.seconds = seconds;
        run.gate_ms = clamp_float(wanted.gate_ms, 0.5f, 500.0f);
        run.fft_size = std::clamp<std::size_t>(wanted.fft_size, 1024, 131'072);
        run.sample_rate = rate;
        run.frames = frames;
        session->measuring = run;

        set_status(status, status_ok());
        return true;
    });
}

extern "C" bool analyzer_session_measure_progress(const AnalyzerSession* session,
                                                  AnalyzerMeasureProgress* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        const analyzer::engine::RecordingProgress recording = session->engine.recording_progress();
        AnalyzerMeasureProgress progress{};
        progress.active = session->measuring.has_value();
        progress.captured = recording.captured;
        progress.total = recording.total;
        progress.complete = session->measuring.has_value() && recording.total > 0 &&
                            recording.captured >= recording.total;
        *out = progress;
        return true;
    });
}

extern "C" void analyzer_session_cancel_measurement(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return;
    }
    guard([&] {
        session->engine.cancel_recording();
        session->measuring.reset();
        session->signal->set(AnalyzerSignal_Silence, -120.0f, 0.0f);
    });
}

// Returns false while the sweep is still playing, so polling this cannot
// deconvolve half a sweep and report the result as a measurement.
extern "C" bool analyzer_session_finish_measurement(AnalyzerSession* session,
                                                    AnalyzerMeasureResult* out,
                                                    AnalyzerStatus* status) noexcept {
    if (session == nullptr) {
        set_status(status, status_failure("null session"));
        return false;
    }
    return guard_status(status, false, [&] {
        if (!session->measuring) {
            set_status(status, status_failure("no measurement is running"));
            return false;
        }
        const analyzer::ffi::MeasurementRun run = *session->measuring;

        std::optional<std::vector<float>> response = session->engine.take_recording();
        if (!response) {
            set_status(status, status_failure("the sweep is still playing"));
            return false;
        }

        session->measuring.reset();
        session->signal->set(AnalyzerSignal_Silence, -120.0f, 0.0f);

        // The stimulus is regenerated rather than recorded. A sweep is
        // deterministic, so this is the same signal that was played, and it
        // costs nothing to keep on the audio thread.
        analyzer::dsp::Generator generator(
            run.sample_rate,
            analyzer::dsp::Signal::sweep(run.start_hz, run.end_hz, run.seconds, 1.0f,
                                         /*repeat=*/false),
            analyzer::ffi::kGeneratorSeed);
        std::vector<float> stimulus(run.frames, 0.0f);
        generator.fill(stimulus);

        analyzer::dsp::Deconvolver deconvolver(run.sample_rate, run.frames);
        std::optional<analyzer::dsp::ImpulseResponse> impulse =
            deconvolver.deconvolve(stimulus, *response, analyzer::dsp::kDefaultRegularisation);
        if (!impulse) {
            set_status(status, status_failure("nothing was recorded - check the input level and "
                                              "that the sweep played"));
            return false;
        }

        const analyzer::dsp::Gate gate = analyzer::dsp::Gate::anechoic(run.gate_ms / 1000.0f);
        std::optional<analyzer::dsp::GatedResponse> gated =
            analyzer::dsp::gated_response(*impulse, gate, run.fft_size);
        if (!gated) {
            set_status(status, status_failure("the gate is longer than the impulse response"));
            return false;
        }

        const std::vector<float> decay = analyzer::dsp::schroeder_decay(*impulse);
        const analyzer::dsp::ReverbTime reverb = analyzer::dsp::reverb_time(decay, run.sample_rate);

        // The peak is the direct arrival. It carries the converter round trip as
        // well as the flight time - see AnalyzerMeasureResult::arrival_ms.
        const float arrival_seconds = impulse->time_at(peak_index(impulse->samples));

        AnalyzerMeasureResult result{};
        result.arrival_ms = arrival_seconds * 1000.0f;
        result.arrival_metres = arrival_seconds * analyzer::ffi::kSpeedOfSound;
        result.peak_amplitude = impulse->peak_amplitude();
        result.edt = reverb.edt.value_or(0.0f);
        result.has_edt = reverb.edt.has_value();
        result.t20 = reverb.t20.value_or(0.0f);
        result.has_t20 = reverb.t20.has_value();
        result.t30 = reverb.t30.value_or(0.0f);
        result.has_t30 = reverb.t30.has_value();
        result.decay_spread = reverb.spread().value_or(0.0f);
        result.has_decay_spread = reverb.spread().has_value();
        result.resolution_hz = gated->resolution_hz;
        result.points = gated->magnitude_db.size();

        analyzer::ffi::Measured measured;
        measured.impulse = *std::move(impulse);
        measured.magnitude_db = std::move(gated->magnitude_db);
        measured.bin_spacing_hz = gated->bin_spacing_hz;
        measured.result = result;
        session->measured = std::move(measured);

        if (out != nullptr) {
            *out = result;
        }
        set_status(status, status_ok());
        return true;
    });
}

extern "C" bool analyzer_session_has_measurement(const AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return false;
    }
    return guard(false, [&] { return session->measured.has_value(); });
}

extern "C" bool analyzer_session_measurement_result(const AnalyzerSession* session,
                                                    AnalyzerMeasureResult* out) noexcept {
    if (session == nullptr || out == nullptr) {
        return false;
    }
    return guard(false, [&] {
        if (!session->measured) {
            return false;
        }
        *out = session->measured->result;
        return true;
    });
}

extern "C" uintptr_t analyzer_session_copy_measured(AnalyzerSession* session, float* out,
                                                    uintptr_t capacity) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        if (!session->measured) {
            return 0;
        }
        const std::size_t columns = std::min(session->columns, static_cast<std::size_t>(capacity));
        const float spacing = session->measured->bin_spacing_hz;
        session->bins = session->measured->magnitude_db;

        analyzer::plot::reduce(session->bins, spacing, session->frequency, columns,
                               session->reduction, session->measured_trace);

        const std::size_t written =
            std::min(session->measured_trace.points.size(), static_cast<std::size_t>(capacity));
        std::copy_n(session->measured_trace.points.begin(), written, out);
        return written;
    });
}

// Values are amplitudes, normalised to the peak, so a caller can draw the
// impulse without knowing the recording level.
extern "C" uintptr_t analyzer_session_copy_impulse(const AnalyzerSession* session, float* out,
                                                   uintptr_t capacity, float seconds) noexcept {
    if (session == nullptr || out == nullptr || capacity == 0 || !std::isfinite(seconds) ||
        seconds <= 0.0f) {
        return 0;
    }
    return guard<std::uintptr_t>(0, [&]() -> std::size_t {
        if (!session->measured) {
            return 0;
        }
        const analyzer::dsp::ImpulseResponse& impulse = session->measured->impulse;
        if (impulse.samples.empty()) {
            return 0;
        }

        const float rate = impulse.sample_rate;
        const std::size_t wanted = std::max<std::size_t>(
            std::min(analyzer::saturating_cast<std::size_t>(std::round(seconds * rate)),
                     impulse.samples.size()),
            1);
        const float peak = std::fmax(impulse.peak_amplitude(), std::numeric_limits<float>::min());

        // Decimate by taking the largest magnitude in each span rather than
        // every nth sample: an impulse response is mostly near zero, and
        // sampling it sparsely would miss the peaks that carry the shape.
        const std::size_t written = std::min<std::size_t>(capacity, wanted);
        for (std::size_t slot = 0; slot < written; ++slot) {
            const std::size_t from = slot * wanted / written;
            const std::size_t to = std::max((slot + 1) * wanted / written, from + 1);
            const std::span<const float> span(impulse.samples.data() + from,
                                              std::min(to, wanted) - from);
            out[slot] = span[peak_index(span)] / peak;
        }
        return written;
    });
}
