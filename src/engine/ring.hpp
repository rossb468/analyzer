// The lock-free crossing from the audio thread to the analysis thread.
//
// A single-producer, single-consumer ring of interleaved float samples. The
// audio callback is the producer; the analysis thread is the consumer.
//
// Why interleaved, and why all-or-nothing
//
// One ring carries interleaved frames rather than one ring per channel. That is
// a correctness decision, not a convenience: with separate rings a partial
// write could advance one channel and not another, and channels that drift
// apart by even one sample destroy a transfer-function phase reading.
// Interleaving makes frame alignment structural.
//
// For the same reason writes are all-or-nothing. If a whole block will not fit,
// the block is dropped and an overrun is counted. A half-written block would
// desynchronise every channel after it.
//
// Overruns are not silent
//
// The producer cannot block - it is on a hard deadline - and it cannot
// allocate. Dropping is the only option left. But a measurement taken across
// dropped audio is wrong rather than merely degraded, so the count is published
// and the UI is expected to surface it. Silent data loss is the worst failure
// mode a measurement tool has.
//
// How it works
//
// Two monotonically increasing counters, `written` and `read`, both in samples.
// The producer owns `written` and only reads `read`; the consumer owns `read`
// and only reads `written`. Each publishes its counter with a release store
// after touching the samples, and loads the other's with an acquire load before
// touching them - so the consumer never sees a counter that has moved past
// samples it cannot see yet, and the producer never overwrites a slot the
// consumer has not finished reading. There is no lock anywhere, and no
// compare-and-swap: with one writer per counter, plain loads and stores are
// enough.

#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <utility>
#include <vector>

namespace analyzer::engine {

namespace detail {

// Shared between the two ends. Each counter sits on its own cache line: they
// are written by different threads, and sharing a line would make every write
// by one thread evict the other's cache - "false sharing", invisible in the
// code and very visible in a profiler.
struct RingState {
    RingState(std::size_t channel_count, std::size_t capacity_samples)
        : channels(channel_count), buffer(capacity_samples, 0.0f) {}

    const std::size_t channels;
    std::vector<float> buffer;

    // 64 bytes covers the cache line of every CPU this runs on. The standard's
    // std::hardware_destructive_interference_size would say the same, but GCC
    // warns that its value is not ABI-stable.
    alignas(64) std::atomic<std::uint64_t> written{0};
    alignas(64) std::atomic<std::uint64_t> read{0};
    alignas(64) std::atomic<std::uint64_t> overruns{0};
};

}  // namespace detail

class CaptureSink;
class CaptureSource;

// Create a capture ring sized for `capacity_frames`.
//
// Sizing: capacity is dictated by worst-case scheduling latency, not by
// throughput. The analysis thread consumes faster than the audio thread
// produces on average, so the only reason to be large is to absorb the
// analysis thread being descheduled. Roughly 8192 frames is ~170 ms of runway
// at 48 kHz and costs 32 KB per channel - memory is not the constraint here,
// overruns are.
//
// `channels` and `capacity_frames` must both be non-zero.
std::pair<CaptureSink, CaptureSource> capture_ring(std::size_t channels,
                                                   std::size_t capacity_frames);

// Audio-thread end of the ring.
//
// Move-only, so there can only ever be one producer: copying it would be a
// compile error, which is the C++ way of making the single-producer
// requirement structural rather than documented.
class CaptureSink {
public:
    CaptureSink(CaptureSink&&) noexcept = default;
    CaptureSink& operator=(CaptureSink&&) noexcept = default;
    CaptureSink(const CaptureSink&) = delete;
    CaptureSink& operator=(const CaptureSink&) = delete;

    // Push one block of interleaved frames.
    //
    // Returns true if the block was accepted. On false the block was dropped
    // whole and overruns() has advanced.
    //
    // Real-time safe: no allocation, no locks, one atomic store to publish.
    bool write_interleaved(std::span<const float> samples) noexcept;

    // Blocks dropped because the analysis thread fell behind.
    std::uint64_t overruns() const noexcept;

    // Channels per frame.
    std::size_t channels() const noexcept { return state_->channels; }

    // Frames that would currently fit.
    std::size_t frames_free() const noexcept;

private:
    friend std::pair<CaptureSink, CaptureSource> capture_ring(std::size_t, std::size_t);
    explicit CaptureSink(std::shared_ptr<detail::RingState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::RingState> state_;
};

// Analysis-thread end of the ring. Move-only, for the same reason.
class CaptureSource {
public:
    CaptureSource(CaptureSource&&) noexcept = default;
    CaptureSource& operator=(CaptureSource&&) noexcept = default;
    CaptureSource(const CaptureSource&) = delete;
    CaptureSource& operator=(const CaptureSource&) = delete;

    // Read whole frames into `destination`, interleaved.
    //
    // Reads min(available, destination.size() / channels) frames and returns
    // that count. Never partial frames.
    std::size_t read_interleaved(std::span<float> destination) noexcept;

    // Whole frames currently readable.
    std::size_t frames_available() const noexcept;

    // Blocks the producer had to drop.
    std::uint64_t overruns() const noexcept;

    // Channels per frame.
    std::size_t channels() const noexcept { return state_->channels; }

private:
    friend std::pair<CaptureSink, CaptureSource> capture_ring(std::size_t, std::size_t);
    explicit CaptureSource(std::shared_ptr<detail::RingState> state) : state_(std::move(state)) {}

    std::shared_ptr<detail::RingState> state_;
};

// Split interleaved frames into per-channel spans.
//
// Allocation-free. Each destination receives min(frames, its length) samples;
// anything beyond that is left untouched. `source` must be a whole number of
// frames for destinations.size() channels.
void deinterleave(std::span<const float> source, std::span<const std::span<float>> destinations);

}  // namespace analyzer::engine
