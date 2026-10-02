// The objects behind the opaque handles.
//
// C sees `struct AnalyzerSession`, `AnalyzerDeviceList` and `AnalyzerTraceStore`
// as incomplete types and only ever holds a pointer; these are the definitions.
// They live in the global namespace because that is where the C names live.
//
// Threading: every member of AnalyzerSession belongs to the thread that calls
// the entry points, in practice the UI thread. The two exceptions are reached
// through objects built to be shared: the SignalState (atomics, read by the
// audio callback) and the equaliser's SnapshotPublisher (the audio callback
// holds the matching reader).

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "audio/backend.hpp"
#include "audio/device.hpp"
#include "audio/stream.hpp"
#include "dsp/deconv.hpp"
#include "dsp/eq.hpp"
#include "dsp/target.hpp"
#include "engine/engine.hpp"
#include "engine/snapshot.hpp"
#include "ffi/eq_processor.hpp"
#include "ffi/internal.hpp"
#include "ffi/signal_state.hpp"
#include "model/measurement.hpp"
#include "model/store.hpp"
#include "plot/axis.hpp"
#include "plot/reduce.hpp"

// A snapshot of the devices present when it was created.
//
// Owning the strings in a list, rather than returning them one at a time, is
// what makes the borrowed `const char*` in AnalyzerDevice safe: they stay valid
// until the list is destroyed.
struct AnalyzerDeviceList {
    std::vector<analyzer::audio::DeviceInfo> devices;
    // Kept alive so the pointers handed out remain valid. An identifier with an
    // embedded NUL cannot be a C string and is carried as empty.
    std::vector<std::pair<std::string, std::string>> strings;
};

namespace analyzer::ffi {

// A sweep in progress.
struct MeasurementRun {
    float start_hz = 0.0f;
    float end_hz = 0.0f;
    float seconds = 0.0f;
    float gate_ms = 0.0f;
    std::size_t fft_size = 0;
    float sample_rate = 0.0f;
    // Samples asked for: the sweep plus enough tail to hold the decay.
    std::size_t frames = 0;
};

// A completed measurement.
struct Measured {
    dsp::ImpulseResponse impulse;
    // Gated magnitude per bin, in decibels.
    std::vector<float> magnitude_db;
    float bin_spacing_hz = 0.0f;
    AnalyzerMeasureResult result{};
};

// Which trace a copy refers to.
enum class Which {
    Live,
    Average,
};

// The latest measurement sampled at the frequencies the plot draws at.
struct MeasuredColumns {
    std::vector<float> frequencies;
    std::vector<float> levels;
};

// How a captured trace is drawn.
struct CapturedTrace {
    model::MeasurementId id;
    bool visible = true;
    // Index into a palette the platform layer owns. The core does not know what
    // colour this is, only that two traces should not share one.
    std::uint32_t colour = 0;
};

// The target a fresh session starts with.
AnalyzerTarget default_target() noexcept;

// The session's newest live spectrum as a measurement, at analysis resolution.
//
// Magnitude only, because that is what an RTA produces - squaring the magnitude
// discarded the phase, and a file claiming zero phase would be
// indistinguishable from one that measured it. The data is empty until
// something has been analysed.
model::Measurement live_measurement(AnalyzerSession& session, std::string name);

// A name argument from C: `fallback` when it is null or not valid UTF-8.
std::string name_or(const char* name, std::string_view fallback);

// Copy up to `capacity` gridlines into `out`, returning how many were written.
std::size_t write_ticks(std::span<const plot::Tick> ticks, AnalyzerTick* out,
                        std::size_t capacity) noexcept;

// Open the device, start the analysis thread and the stream, and return the
// session that owns them.
//
// `uid` names the device, or is empty for the system default input. Everything
// that can go wrong - a bad configuration, a missing device, a stream the
// hardware refuses - is thrown as std::runtime_error carrying the message the
// caller should see. The backend is a parameter so that tests can drive a
// session from the offline one; the entry point passes default_backend().
std::unique_ptr<AnalyzerSession> start_session(const AnalyzerSessionConfig& config,
                                               const std::optional<std::string>& uid,
                                               audio::AudioBackend& backend);

}  // namespace analyzer::ffi

// A running capture and analysis session.
//
// `engine` is declared before `stream` on purpose: members are destroyed in
// reverse, so the stream - and the audio callback inside it, which holds the
// ring's write end - goes before the analysis thread that reads the ring.
struct AnalyzerSession {
    AnalyzerSession(analyzer::engine::Engine engine,
                    std::unique_ptr<analyzer::audio::AudioStream> stream, float sample_rate,
                    std::shared_ptr<analyzer::ffi::SignalState> signal,
                    analyzer::engine::SnapshotPublisher<analyzer::ffi::EqCoefficients> eq_publisher,
                    std::string device_name);

    // Stop the stream, then the analysis thread. Idempotent; the entry point
    // that destroys a session calls it before the destructor runs.
    void stop() noexcept;

    // Describe the target for the UI.
    AnalyzerTarget target_description() const;

    // Whether a custom curve has been loaded and has points in it.
    bool has_custom_target() const noexcept;

    // Apply a shape chosen in the UI.
    void apply_target(const AnalyzerTarget& wanted);

    // The latest measurement sampled at the frequencies the plot draws at.
    //
    // Both alignment and the fit work from this rather than from raw bins, so
    // what they operate on is what is on screen, and the points are log-spaced -
    // which is what makes every octave carry equal weight in the fit rather
    // than the top one dominating.
    std::optional<analyzer::ffi::MeasuredColumns> measured_at_columns();

    // Align the target to the latest analysed frame.
    bool align_target();

    // The equaliser the current mode selects, or null when it is off.
    const analyzer::dsp::Equaliser* equaliser() const noexcept;
    analyzer::dsp::Equaliser* equaliser() noexcept;

    // Push the active equaliser's coefficients to the audio thread.
    //
    // Called after every change. Cheap - a couple of dozen biquad designs - and
    // it happens on a UI event, not per frame.
    void publish_eq();

    // Centre frequency of every pixel column, cached.
    std::span<const float> columns_hz(std::size_t columns);

    // Shared body of the copy calls: reduce the live or average trace onto the
    // plot's columns and copy at most `capacity` of them into `out`.
    std::size_t copy_reduced(float* out, std::size_t capacity, analyzer::ffi::Which which);

    analyzer::engine::Engine engine;
    std::unique_ptr<analyzer::audio::AudioStream> stream;
    analyzer::plot::FrequencyAxis frequency;
    analyzer::plot::LevelAxis level;
    analyzer::plot::Reduction reduction = analyzer::plot::Reduction::Max;
    std::size_t columns = 1000;
    analyzer::plot::Trace trace;
    analyzer::plot::Trace average_trace;
    analyzer::plot::Trace transfer_trace;
    // Axis for phase in degrees, spanning the same pixels as the level axis.
    // Held here so Swift never converts degrees to pixels itself.
    analyzer::plot::LevelAxis phase;
    // Axis for coherence, 0..1, over the same pixels again.
    analyzer::plot::LevelAxis coherence;
    std::shared_ptr<analyzer::ffi::SignalState> signal;
    // The two equalisers are both kept, so switching between them does not
    // throw away the one being left.
    analyzer::dsp::Equaliser graphic;
    analyzer::dsp::Equaliser parametric;
    AnalyzerEqMode eq_mode = AnalyzerEqMode_Off;
    analyzer::engine::SnapshotPublisher<analyzer::ffi::EqCoefficients> eq_publisher;
    std::uint64_t eq_generation = 0;
    // Centre frequency of each pixel column, for evaluating the equaliser.
    // Rebuilt only when the geometry changes.
    std::vector<float> column_hz;
    analyzer::plot::Trace eq_trace;
    // The response a correction is aiming at, and its alignment offset.
    analyzer::dsp::TargetCurve target;
    analyzer::plot::Trace target_trace;
    // Frequency axis for the spectrogram, which is as tall as the drawable
    // rather than as wide, and so needs its own.
    analyzer::plot::FrequencyAxis spectrogram_axis;
    analyzer::plot::Trace spectrogram_column;
    // The sweep currently being played, if a measurement is running.
    std::optional<analyzer::ffi::MeasurementRun> measuring;
    // The last completed measurement.
    std::optional<analyzer::ffi::Measured> measured;
    analyzer::plot::Trace measured_trace;
    std::vector<float> bins;
    // Scratch for the distortion analysis, which needs linear power.
    std::vector<float> power;
    std::string device_name;
};

// Captured curves, held independently of any session.
//
// The store is a separate handle rather than part of a session, because the
// whole point of a captured trace is to compare it against something measured
// later - including after a change that restarts the session. Transform size,
// window and averaging all restart it, and holding traces inside would mean
// capturing a "before" curve and then losing it the moment you changed the
// setting you wanted to compare.
//
// Traces are stored as measurements at analysis resolution, not as the pixel
// columns they were drawn as. Storing the reduced curve would be storing a
// picture: it would stretch rather than re-reduce when the window resized, and
// a trace captured at one axis range would be wrong at any other.
struct AnalyzerTraceStore {
    analyzer::model::MeasurementStore measurements;
    std::vector<analyzer::ffi::CapturedTrace> display;
    // Next palette index to hand out. Monotonic, so two traces captured either
    // side of a deletion do not end up the same colour.
    std::uint32_t next_colour = 0;
    // Scratch for reduction, reused so drawing does not allocate per frame.
    analyzer::plot::Trace reduced;
    std::vector<float> bins;
};
