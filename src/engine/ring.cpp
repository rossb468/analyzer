#include "engine/ring.hpp"

#include <algorithm>

#include "base/contract.hpp"

namespace analyzer::engine {

namespace {

// Copy `count` samples into the ring starting at absolute position `position`,
// wrapping at the end of the buffer. At most two contiguous copies.
void copy_in(std::vector<float>& ring, std::uint64_t position, const float* from,
             std::size_t count) noexcept {
    const std::size_t capacity = ring.size();
    const auto start = static_cast<std::size_t>(position % capacity);
    const std::size_t first = std::min(count, capacity - start);
    std::copy_n(from, first, ring.begin() + static_cast<std::ptrdiff_t>(start));
    std::copy_n(from + first, count - first, ring.begin());
}

// The mirror image: copy `count` samples out of the ring.
void copy_out(const std::vector<float>& ring, std::uint64_t position, float* to,
              std::size_t count) noexcept {
    const std::size_t capacity = ring.size();
    const auto start = static_cast<std::size_t>(position % capacity);
    const std::size_t first = std::min(count, capacity - start);
    std::copy_n(ring.begin() + static_cast<std::ptrdiff_t>(start), first, to);
    std::copy_n(ring.begin(), count - first, to + first);
}

}  // namespace

std::pair<CaptureSink, CaptureSource> capture_ring(std::size_t channels,
                                                   std::size_t capacity_frames) {
    ANALYZER_EXPECTS(channels > 0, "ring must carry at least one channel");
    ANALYZER_EXPECTS(capacity_frames > 0, "ring must hold at least one frame");
    auto state = std::make_shared<detail::RingState>(channels, channels * capacity_frames);
    return {CaptureSink(state), CaptureSource(state)};
}

bool CaptureSink::write_interleaved(std::span<const float> samples) noexcept {
    if (samples.empty()) {
        return true;
    }
    auto& state = *state_;
    // A partial frame means the caller's channel count is wrong. Refuse it
    // rather than corrupting alignment for everything that follows.
    if (samples.size() % state.channels != 0) {
        state.overruns.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    // Our own counter: only this thread writes it, so relaxed is enough.
    const std::uint64_t written = state.written.load(std::memory_order_relaxed);
    // The consumer's counter: acquire, so its reads of the slots it has
    // released are complete before we overwrite them.
    const std::uint64_t read = state.read.load(std::memory_order_acquire);
    const auto free = state.buffer.size() - static_cast<std::size_t>(written - read);
    if (free < samples.size()) {
        state.overruns.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    copy_in(state.buffer, written, samples.data(), samples.size());
    // Release: the samples above become visible to the consumer no later than
    // the counter that tells it they exist.
    state.written.store(written + samples.size(), std::memory_order_release);
    return true;
}

std::uint64_t CaptureSink::overruns() const noexcept {
    return state_->overruns.load(std::memory_order_relaxed);
}

std::size_t CaptureSink::frames_free() const noexcept {
    const auto& state = *state_;
    const std::uint64_t used =
        state.written.load(std::memory_order_relaxed) - state.read.load(std::memory_order_acquire);
    return (state.buffer.size() - static_cast<std::size_t>(used)) / state.channels;
}

std::size_t CaptureSource::read_interleaved(std::span<float> destination) noexcept {
    auto& state = *state_;
    const std::size_t frames = std::min(destination.size() / state.channels, frames_available());
    if (frames == 0) {
        return 0;
    }
    const std::size_t count = frames * state.channels;
    const std::uint64_t read = state.read.load(std::memory_order_relaxed);

    copy_out(state.buffer, read, destination.data(), count);
    // Release: our reads of these slots are finished before the producer can
    // see that it may overwrite them.
    state.read.store(read + count, std::memory_order_release);
    return frames;
}

std::size_t CaptureSource::frames_available() const noexcept {
    const auto& state = *state_;
    // Acquire on the producer's counter pairs with its release store, making
    // every sample it counts visible here.
    const std::uint64_t available =
        state.written.load(std::memory_order_acquire) - state.read.load(std::memory_order_relaxed);
    return static_cast<std::size_t>(available) / state.channels;
}

std::uint64_t CaptureSource::overruns() const noexcept {
    return state_->overruns.load(std::memory_order_relaxed);
}

void deinterleave(std::span<const float> source, std::span<const std::span<float>> destinations) {
    const std::size_t channels = destinations.size();
    ANALYZER_EXPECTS(channels > 0, "need at least one destination channel");
    ANALYZER_EXPECTS(source.size() % channels == 0, "source must be a whole number of frames");

    const std::size_t frames = source.size() / channels;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        const auto out = destinations[channel];
        const std::size_t take = std::min(frames, out.size());
        for (std::size_t frame = 0; frame < take; ++frame) {
            out[frame] = source[frame * channels + channel];
        }
    }
}

}  // namespace analyzer::engine
