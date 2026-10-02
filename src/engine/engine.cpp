#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <mutex>

#if defined(__APPLE__) || defined(__linux__)
#include <pthread.h>
#endif

#include "base/contract.hpp"
#include "dsp/delay.hpp"
#include "dsp/transfer.hpp"
#include "engine/delay_line.hpp"

namespace analyzer::engine {

namespace {

// How long the worker sleeps when the ring is empty.
//
// A compromise. Blocking on a condition variable would need the audio thread to
// signal it, and signalling is not something a thread on a hard deadline should
// do. Spinning would burn a core. At 48 kHz a 128-frame block arrives every
// 2.7 ms, so polling at 1 ms adds well under a millisecond of latency and costs
// almost nothing, since the machine is already awake servicing audio.
constexpr auto kIdlePoll = std::chrono::milliseconds(1);

// Frames read from the ring per iteration.
//
// This bound is load-bearing, not a tuning knob. Draining until the ring is
// empty looks natural and is wrong: a producer that keeps the ring topped up
// means the drain loop never exits and nothing is ever published. Reading a
// bounded amount and then publishing guarantees the UI sees progress no matter
// how fast audio arrives. Test: AFastProducerCannotStarvePublication.
constexpr std::size_t kDrainFrames = 4096;

// Name the worker so it is recognisable in a debugger or Instruments. Only
// possible where the platform offers a call for it; elsewhere a no-op.
void name_this_thread() noexcept {
#if defined(__APPLE__)
    pthread_setname_np("analyzer-analysis");
#elif defined(__linux__)
    pthread_setname_np(pthread_self(), "analyzer-analysis");
#endif
}

}  // namespace

// A raw capture in progress, shared between the analysis thread and the
// caller.
//
// A mutex is fine and a ring is not needed: this is shared between the
// *analysis* thread and the caller, never the audio callback. The callback
// still only writes into the lock-free ring and returns. The lock is held for
// one append of at most kDrainFrames samples, and only while a measurement is
// running.
struct Recording {
    std::vector<float> samples;
    // Frames still wanted. Zero with `active` set means the capture is done.
    std::size_t wanted = 0;
    // Frames asked for, kept so progress can be reported as a fraction.
    std::size_t total = 0;
    bool active = false;
};

struct Engine::Shared {
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> published{0};
    // A flag rather than a queue: the worker checks it once per iteration,
    // and a missed reset would be indistinguishable from a late one.
    std::atomic<bool> reset_average{false};
    std::atomic<std::uint32_t> applied_delay{0};
    std::atomic<bool> estimate_delay{false};

    mutable std::mutex recording_mutex;
    Recording recording;
};

namespace {

// Everything the analysis thread owns. Built on the caller's thread, where
// allocating is fine, then moved onto the worker and never resized.
class Worker {
public:
    Worker(const EngineConfig& config, CaptureSource source,
           SnapshotPublisher<SpectrumFrame> publisher, std::shared_ptr<Engine::Shared> shared)
        : source_(std::move(source)),
          publisher_(std::move(publisher)),
          shared_(std::move(shared)),
          analyzer_(config.spectrum),
          // Same transform, different averaging. Sharing the configuration is
          // what keeps the two traces directly comparable.
          average_(with_averaging(config.spectrum, config.average)),
          mode_(config.mode),
          channels_(config.channels),
          // In transfer mode the spectrum follows the measurement channel, so
          // the level shown is the level being measured rather than the
          // stimulus.
          analysed_(config.mode.is_transfer() ? config.mode.measurement_channel
                                              : config.analysis_channel),
          interleaved_(kDrainFrames * config.channels, 0.0f),
          mono_(kDrainFrames, 0.0f),
          raw_reference_(kDrainFrames, 0.0f),
          reference_(kDrainFrames, 0.0f),
          // A second of delay covers 343 m of air, which is more room than
          // anyone measures, and costs 192 kB at 48 kHz.
          delay_line_(static_cast<std::size_t>(std::max(config.spectrum.sample_rate, 1.0f))) {
        // Only built when needed: a transfer function is two more FFTs per
        // frame, and a spectrum-only session should not pay for them.
        if (mode_.is_transfer()) {
            transfer_.emplace(dsp::TransferConfig{
                .sample_rate = config.spectrum.sample_rate,
                .size = config.spectrum.size,
                .window = config.spectrum.window,
                .overlap = config.spectrum.overlap,
                .averaging = dsp::TransferAveraging::exponential(0.15f),
            });
            finder_.emplace(dsp::DelayFinder::acoustic(config.spectrum.sample_rate));
            finder_reference_.assign(finder_->size(), 0.0f);
            finder_measurement_.assign(finder_->size(), 0.0f);
        }
    }

    // The frame every snapshot buffer starts as, sized so that publishing
    // never has to grow a vector.
    static SpectrumFrame initial_frame(const EngineConfig& config) {
        const dsp::SpectrumAnalyzer probe(config.spectrum);
        SpectrumFrame frame;
        frame.bins.assign(probe.bins(), dsp::kSpectrumFloorDb);
        frame.average_bins.assign(probe.bins(), dsp::kSpectrumFloorDb);
        frame.bin_spacing_hz = probe.bin_spacing_hz();
        frame.sample_rate = config.spectrum.sample_rate;
        // Reserved, not sized: in spectrum mode these stay empty, and empty is
        // how a reader knows there is no transfer curve.
        if (config.mode.is_transfer()) {
            frame.transfer_magnitude_db.reserve(probe.bins());
            frame.transfer_phase_degrees.reserve(probe.bins());
            frame.transfer_coherence.reserve(probe.bins());
        }
        return frame;
    }

    void run() {
        name_this_thread();
        std::uint64_t sequence = 0;
        auto& shared = *shared_;

        while (!shared.stop.load(std::memory_order_relaxed)) {
            if (shared.reset_average.exchange(false, std::memory_order_relaxed)) {
                average_.reset();
            }

            // Exactly one bounded pass per iteration. See kDrainFrames: looping
            // until the ring is empty lets a fast producer starve publication.
            const std::size_t frames = source_.read_interleaved(interleaved_);
            if (frames == 0) {
                std::this_thread::sleep_for(kIdlePoll);
                continue;
            }

            // Extract the channel under analysis. Interleaved storage is what
            // keeps a two-channel transfer function sample aligned: both
            // channels are lifted out of the same block, so no amount of
            // scheduling jitter can slide one against the other.
            extract(analysed_, frames, mono_);
            capture(frames);

            if (mode_.is_transfer()) {
                extract(mode_.reference_channel, frames, raw_reference_);
                estimate_delay(frames);

                // Delay the reference, not the measurement. Sound takes time
                // to reach the microphone, so the reference is the early one,
                // and holding it back is what makes the two describe the same
                // instant.
                const auto delay = shared.applied_delay.load(std::memory_order_relaxed);
                delay_line_.process(std::span(raw_reference_).first(frames),
                                    std::span(reference_).first(frames), delay);
                transfer_->push(std::span(reference_).first(frames),
                                std::span(mono_).first(frames));
            }

            const auto mono = std::span<const float>(mono_).first(frames);
            const std::size_t produced = analyzer_.push(mono);
            // Both see the same samples, so the two traces describe the same
            // audio and any difference between them is averaging alone.
            average_.push(mono);

            if (produced > 0) {
                ++sequence;
                publish(sequence);
                shared.published.store(sequence, std::memory_order_relaxed);
            }
        }
    }

private:
    static dsp::SpectrumConfig with_averaging(dsp::SpectrumConfig config,
                                              dsp::Averaging averaging) {
        config.averaging = averaging;
        return config;
    }

    // Copy one channel of the interleaved block into `out`.
    void extract(std::size_t channel, std::size_t frames, std::vector<float>& out) noexcept {
        for (std::size_t frame = 0; frame < frames; ++frame) {
            out[frame] = interleaved_[frame * channels_ + channel];
        }
    }

    // Copy the analysed channel into an armed recording.
    //
    // Runs on the analysis thread, never the audio callback, so taking the
    // lock here is allowed. It is uncontended except when the caller polls
    // progress. The append never allocates: begin_recording() reserved the
    // whole capture.
    void capture(std::size_t frames) {
        auto& shared = *shared_;
        const std::scoped_lock lock(shared.recording_mutex);
        auto& recording = shared.recording;
        if (!recording.active || recording.wanted == 0) {
            return;
        }
        const std::size_t take = std::min(frames, recording.wanted);
        recording.samples.insert(recording.samples.end(), mono_.begin(),
                                 mono_.begin() + static_cast<std::ptrdiff_t>(take));
        recording.wanted -= take;
    }

    // Re-measure the reference-to-measurement delay, if asked to.
    //
    // Cross-correlation needs a window far longer than one drain pass, so
    // blocks are accumulated until the finder has enough. The request flag is
    // only cleared once an answer is produced, so asking during silence waits
    // for signal rather than returning zero.
    void estimate_delay(std::size_t frames) noexcept {
        auto& shared = *shared_;
        if (!shared.estimate_delay.load(std::memory_order_relaxed)) {
            finder_filled_ = 0;
            return;
        }
        if (!finder_) {
            shared.estimate_delay.store(false, std::memory_order_relaxed);
            return;
        }

        const std::size_t take = std::min(finder_->size() - finder_filled_, frames);
        const auto at = static_cast<std::ptrdiff_t>(finder_filled_);
        std::copy_n(raw_reference_.begin(), take, finder_reference_.begin() + at);
        std::copy_n(mono_.begin(), take, finder_measurement_.begin() + at);
        finder_filled_ += take;

        if (finder_filled_ < finder_->size()) {
            return;
        }
        finder_filled_ = 0;

        if (const auto estimate = finder_->find(finder_reference_, finder_measurement_)) {
            // A negative delay means the measurement arrived first, which is
            // physically impossible for an acoustic path and in practice means
            // the two channels are swapped. Clamping to zero is honest: the
            // curve then plainly shows the problem.
            const float samples = std::round(std::max(estimate->samples, 0.0f));
            const auto capped =
                std::min(static_cast<double>(samples), static_cast<double>(delay_line_.capacity()));
            shared.applied_delay.store(static_cast<std::uint32_t>(capped),
                                       std::memory_order_relaxed);
            shared.estimate_delay.store(false, std::memory_order_relaxed);
        }
    }

    void publish(std::uint64_t sequence) {
        const std::uint64_t overruns = source_.overruns();
        const auto delay = shared_->applied_delay.load(std::memory_order_relaxed);
        publisher_.publish_with([&](SpectrumFrame& frame) {
            // The pending buffer is recycled and holds a value from two
            // publishes ago, so every field is overwritten. The vectors were
            // sized or reserved in initial_frame(), so resizing them here does
            // not allocate.
            frame.sequence = sequence;
            frame.bins.resize(analyzer_.bins());
            analyzer_.write_db_fs(frame.bins);
            frame.average_bins.resize(average_.bins());
            average_.write_db_fs(frame.average_bins);
            frame.average_frames = average_.frames();
            frame.bin_spacing_hz = analyzer_.bin_spacing_hz();
            frame.sample_rate = analyzer_.bin_spacing_hz() * static_cast<float>(analyzer_.size());
            frame.frames_averaged = analyzer_.frames();
            frame.overruns = overruns;

            if (transfer_) {
                const std::size_t bins = transfer_->bins();
                frame.transfer_magnitude_db.resize(bins);
                frame.transfer_phase_degrees.resize(bins);
                frame.transfer_coherence.resize(bins);
                transfer_->write_magnitude_db(frame.transfer_magnitude_db);
                transfer_->write_phase_degrees(frame.transfer_phase_degrees);
                transfer_->write_coherence(frame.transfer_coherence);
                frame.transfer_delay_frames = delay;
                frame.transfer_frames = transfer_->frames();
            } else {
                // The pending buffer is recycled, so leaving these alone could
                // leave a stale curve behind.
                frame.transfer_magnitude_db.clear();
                frame.transfer_phase_degrees.clear();
                frame.transfer_coherence.clear();
                frame.transfer_frames = 0;
                frame.transfer_delay_frames = 0;
            }
        });
    }

    CaptureSource source_;
    SnapshotPublisher<SpectrumFrame> publisher_;
    std::shared_ptr<Engine::Shared> shared_;
    dsp::SpectrumAnalyzer analyzer_;
    dsp::SpectrumAnalyzer average_;
    std::optional<dsp::TransferFunction> transfer_;
    AnalysisMode mode_;
    std::size_t channels_;
    std::size_t analysed_;

    std::vector<float> interleaved_;
    std::vector<float> mono_;
    // Reference channel as captured, before delay compensation.
    std::vector<float> raw_reference_;
    // Reference channel after delay compensation. What the transfer sees.
    std::vector<float> reference_;
    DelayLine delay_line_;

    std::optional<dsp::DelayFinder> finder_;
    // Accumulators for the delay finder, which needs a longer view than one
    // drain pass provides.
    std::vector<float> finder_reference_;
    std::vector<float> finder_measurement_;
    std::size_t finder_filled_ = 0;
};

}  // namespace

EngineConfig EngineConfig::mono(float sample_rate) {
    EngineConfig config;
    config.spectrum.sample_rate = sample_rate;
    return config;
}

std::pair<CaptureSink, Engine> Engine::start(const EngineConfig& config) {
    ANALYZER_EXPECTS(config.channels > 0, "engine needs at least one channel");
    ANALYZER_EXPECTS(config.analysis_channel < config.channels,
                     "analysis channel does not exist among the configured channels");
    if (config.mode.is_transfer()) {
        ANALYZER_EXPECTS(config.mode.reference_channel < config.channels &&
                             config.mode.measurement_channel < config.channels,
                         "transfer channels do not exist among the configured channels");
        ANALYZER_EXPECTS(config.mode.reference_channel != config.mode.measurement_channel,
                         "reference and measurement must be different channels; the same "
                         "channel against itself is a wire, not a measurement");
    }

    auto [sink, source] = capture_ring(config.channels, config.ring_capacity_frames);
    auto [publisher, reader] = snapshot_channel(Worker::initial_frame(config));
    auto shared = std::make_shared<Shared>();

    // The worker is built here, where allocating is allowed, and then moved
    // onto its thread whole.
    auto worker = std::make_unique<Worker>(config, std::move(source), std::move(publisher), shared);
    std::thread thread([worker = std::move(worker)] { worker->run(); });

    return {std::move(sink),
            Engine(config, std::move(shared), std::move(reader), std::move(thread))};
}

Engine::Engine(EngineConfig config, std::shared_ptr<Shared> shared,
               SnapshotReader<SpectrumFrame> reader, std::thread worker)
    : config_(std::move(config)),
      shared_(std::move(shared)),
      reader_(std::move(reader)),
      worker_(std::move(worker)) {}

Engine::Engine(Engine&&) noexcept = default;

Engine& Engine::operator=(Engine&& other) noexcept {
    if (this != &other) {
        stop();
        config_ = std::move(other.config_);
        shared_ = std::move(other.shared_);
        reader_ = std::move(other.reader_);
        worker_ = std::move(other.worker_);
    }
    return *this;
}

Engine::~Engine() {
    stop();
}

void Engine::stop() {
    if (shared_) {
        shared_->stop.store(true, std::memory_order_relaxed);
    }
    if (worker_.joinable()) {
        worker_.join();
    }
}

void Engine::begin_recording(std::size_t frames) {
    // Allocate before taking the lock, so the analysis thread is never held up
    // behind the allocator.
    std::vector<float> samples;
    samples.reserve(frames);

    const std::scoped_lock lock(shared_->recording_mutex);
    auto& recording = shared_->recording;
    recording.samples = std::move(samples);
    recording.wanted = frames;
    recording.total = frames;
    recording.active = true;
}

RecordingProgress Engine::recording_progress() const {
    const std::scoped_lock lock(shared_->recording_mutex);
    const auto& recording = shared_->recording;
    if (!recording.active) {
        return {};
    }
    return {recording.samples.size(), recording.total};
}

std::optional<std::vector<float>> Engine::take_recording() {
    const std::scoped_lock lock(shared_->recording_mutex);
    auto& recording = shared_->recording;
    if (!recording.active || recording.wanted > 0) {
        return std::nullopt;
    }
    recording.active = false;
    recording.total = 0;
    return std::exchange(recording.samples, {});
}

void Engine::cancel_recording() {
    // Swap the buffer out under the lock and free it after, for the same
    // reason begin_recording() allocates before locking.
    Recording released;
    {
        const std::scoped_lock lock(shared_->recording_mutex);
        std::swap(released, shared_->recording);
    }
}

std::uint64_t Engine::published_count() const noexcept {
    return shared_->published.load(std::memory_order_relaxed);
}

void Engine::reset_average() noexcept {
    shared_->reset_average.store(true, std::memory_order_relaxed);
}

std::uint32_t Engine::reference_delay() const noexcept {
    return shared_->applied_delay.load(std::memory_order_relaxed);
}

void Engine::set_reference_delay(std::uint32_t samples) noexcept {
    shared_->applied_delay.store(samples, std::memory_order_relaxed);
}

void Engine::estimate_reference_delay() noexcept {
    shared_->estimate_delay.store(true, std::memory_order_relaxed);
}

bool Engine::delay_estimate_pending() const noexcept {
    return shared_->estimate_delay.load(std::memory_order_relaxed);
}

}  // namespace analyzer::engine
