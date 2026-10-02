// The analysis worker and the graph it runs.
//
// ring.hpp and snapshot.hpp are plumbing; this is the thing that owns a thread
// and connects them. It sits between an audio callback and a user interface:
//
//   audio thread          ring          analysis thread     triple buffer     UI thread
//   ------------                        ---------------                       ---------
//   hard deadline    -->  [ring]  -->   heavy FFT work  -->  [snapshot]  -->  draws
//   deinterleave                        no deadline,          newest wins      never
//   and return                          must keep up                          blocks
//
// 1. Engine::start() hands back a CaptureSink for the audio callback and spawns
//    a worker thread.
// 2. The worker drains the ring, feeds the spectrum analyzer (and in transfer
//    mode the transfer function), and publishes a SpectrumFrame whenever new
//    frames complete.
// 3. The UI reads the newest frame whenever it feels like drawing.
//
// Each hand-off is lock-free for a specific reason. The audio thread cannot
// take a lock, because a lower-priority thread holding it would invert priority
// and blow the deadline. The UI must never be able to stall analysis. And
// nothing anywhere may allocate on the audio side.
//
// Reconfiguration
// ---------------
// There is none. Changing FFT size, window or channel count means destroying
// the engine and starting another. Live reconfiguration would need the analysis
// buffers resized underneath a running audio callback, and the resulting
// synchronisation is not worth it for something a user does by clicking a menu.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "dsp/spectrum.hpp"
#include "engine/ring.hpp"
#include "engine/snapshot.hpp"

namespace analyzer::engine {

// One published analysis result.
//
// The worker fills this in place inside the triple buffer, so once running a
// frame costs no allocation.
struct SpectrumFrame {
    // Increments once per publication. Lets a reader tell a repeat from a
    // genuinely new frame, and makes a torn read detectable in tests.
    std::uint64_t sequence = 0;
    // Level per bin in dBFS, 0 dBFS being a full-scale sine.
    //
    // The live trace: whatever averaging the user asked for, usually short so
    // it tracks what is happening now.
    std::vector<float> bins;
    // The same spectrum under long-term averaging, accumulated independently.
    //
    // A separate analyzer rather than a smoothed copy of `bins`. Smoothing the
    // already-averaged live trace would compound two time constants and give a
    // curve that is neither responsive nor statistically better; two analyzers
    // over the same samples give a genuinely lower-variance estimate while the
    // live trace stays fast.
    std::vector<float> average_bins;
    // Frames folded into the long-term average, which is what makes it
    // trustworthy. Reported so a UI can show progress rather than an
    // indistinguishable curve.
    std::uint32_t average_frames = 0;
    // Hertz between adjacent bins.
    float bin_spacing_hz = 0.0f;
    // Rate the analysis ran at.
    float sample_rate = 0.0f;
    // Frames folded into the current live average.
    std::uint32_t frames_averaged = 0;
    // Blocks the audio callback had to drop. Non-zero invalidates the
    // measurement and the UI is expected to say so.
    std::uint64_t overruns = 0;

    // Transfer function magnitude per bin, in decibels. Empty in spectrum
    // mode. Kept on the same frame rather than published separately so a UI
    // never draws a magnitude from one instant beside a coherence from another.
    std::vector<float> transfer_magnitude_db;
    // Transfer function phase per bin, in degrees.
    std::vector<float> transfer_phase_degrees;
    // Coherence per bin, 0 to 1.
    std::vector<float> transfer_coherence;
    // Samples of delay currently applied to the reference channel.
    std::uint32_t transfer_delay_frames = 0;
    // Frames folded into the transfer function average.
    //
    // Coherence is identically 1 for a single frame, so a UI should refuse to
    // draw it until this is comfortably above one.
    std::uint32_t transfer_frames = 0;

    // Centre frequency of bin `index`.
    float bin_frequency(std::size_t index) const noexcept {
        return static_cast<float>(index) * bin_spacing_hz;
    }
};

// What the engine computes.
struct AnalysisMode {
    enum class Kind {
        // A single-channel spectrum: the live trace and its long-term average.
        Spectrum,
        // A two-channel transfer function alongside the spectrum. The spectrum
        // still runs, on the measurement channel, because a user wants to see
        // the raw level as well as the response.
        Transfer,
    };

    Kind kind = Kind::Spectrum;
    // Transfer only: the channel carrying what was sent.
    std::size_t reference_channel = 0;
    // Transfer only: the channel carrying what was heard.
    std::size_t measurement_channel = 0;

    static constexpr AnalysisMode spectrum() { return {}; }
    static constexpr AnalysisMode transfer(std::size_t reference, std::size_t measurement) {
        return {Kind::Transfer, reference, measurement};
    }

    bool is_transfer() const noexcept { return kind == Kind::Transfer; }

    friend constexpr bool operator==(const AnalysisMode&, const AnalysisMode&) = default;
};

// How to set the engine up.
struct EngineConfig {
    // Channels the audio callback will deliver, interleaved.
    std::size_t channels = 1;
    // Which of them to analyse in spectrum mode.
    std::size_t analysis_channel = 0;
    // Spectrum settings, including sample rate and FFT size.
    dsp::SpectrumConfig spectrum;
    // Averaging for the long-term trace. Everything else is taken from
    // `spectrum`, so the two analyzers differ only in how they average.
    dsp::Averaging average = dsp::Averaging::infinite();
    // What to compute.
    AnalysisMode mode;
    // Ring capacity. Sized by worst-case scheduling latency, not throughput -
    // 8192 frames is roughly 170 ms of runway at 48 kHz.
    std::size_t ring_capacity_frames = 8192;

    // Single-channel capture at the given rate, with sensible defaults.
    static EngineConfig mono(float sample_rate);
};

// How far a raw capture has got.
struct RecordingProgress {
    std::size_t captured = 0;
    std::size_t total = 0;

    friend constexpr bool operator==(const RecordingProgress&, const RecordingProgress&) = default;
};

// Owns the analysis thread and exposes the newest result.
//
// Move-only. Destroying it stops the worker and joins it. All methods are for
// the thread that owns the Engine (in practice the UI thread); the CaptureSink
// returned beside it is the only part that goes to the audio thread.
class Engine {
public:
    // Spawn the worker and return it alongside the sink for the audio callback.
    //
    // The sink is deliberately separate: it is the only part that crosses onto
    // the real-time thread, and it is move-only, so a second producer cannot
    // exist.
    //
    // The configuration must be consistent - at least one channel, an analysis
    // channel that exists, distinct transfer channels that exist, a non-empty
    // ring - or this aborts.
    static std::pair<CaptureSink, Engine> start(const EngineConfig& config);

    Engine(Engine&&) noexcept;
    Engine& operator=(Engine&&) noexcept;
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;
    ~Engine();

    // Arm a raw capture of `frames` samples from the analysed channel.
    //
    // Swept measurement needs the recorded samples themselves, not a spectrum.
    // The buffer is allocated here, on the caller's thread, so the analysis
    // thread only ever appends into space that already exists. Any capture
    // already in progress is discarded.
    void begin_recording(std::size_t frames);

    // Samples captured so far, and how many were asked for. {0, 0} when
    // nothing is armed.
    RecordingProgress recording_progress() const;

    // Take the recording once it is complete.
    //
    // Returns nullopt while it is still filling, so a caller polling this
    // cannot accidentally deconvolve half a sweep and report the result as a
    // measurement.
    std::optional<std::vector<float>> take_recording();

    // Abandon a capture in progress and release its buffer.
    void cancel_recording();

    // The newest published frame. Never blocks. Valid until the next call.
    const SpectrumFrame& latest() noexcept { return reader_.read(); }

    // Whether a new frame has arrived since the last latest().
    //
    // Lets a UI skip a redraw entirely when nothing changed, which is how the
    // idle-CPU target gets met - a timer redrawing identical data is the real
    // battery cost.
    bool has_new_frame() const noexcept { return reader_.has_update(); }

    // Total frames published since start, readable without touching the
    // snapshot. Useful for tests and for a throughput readout.
    std::uint64_t published_count() const noexcept;

    // Restart the long-term average without disturbing the live trace.
    //
    // Takes effect on the worker's next pass, so a caller should not expect
    // the very next frame to show a cleared average.
    void reset_average() noexcept;

    // Delay applied to the reference channel, in samples.
    std::uint32_t reference_delay() const noexcept;

    // Set the reference delay by hand, in samples.
    void set_reference_delay(std::uint32_t samples) noexcept;

    // Ask the worker to measure the delay and apply it.
    //
    // Takes effect once enough signal has passed through, so this returns
    // immediately and the answer appears on a later frame. Requesting during
    // silence simply waits, which is better than answering zero.
    void estimate_reference_delay() noexcept;

    // Whether a delay estimate is still pending.
    bool delay_estimate_pending() const noexcept;

    // The configuration in force.
    const EngineConfig& config() const noexcept { return config_; }

    // Stop the worker and wait for it. Idempotent; the destructor calls it too.
    void stop();

    // State shared between this object and the worker thread. Defined in
    // engine.cpp.
    struct Shared;

private:
    Engine(EngineConfig config, std::shared_ptr<Shared> shared,
           SnapshotReader<SpectrumFrame> reader, std::thread worker);

    EngineConfig config_;
    std::shared_ptr<Shared> shared_;
    SnapshotReader<SpectrumFrame> reader_;
    std::thread worker_;
};

}  // namespace analyzer::engine
