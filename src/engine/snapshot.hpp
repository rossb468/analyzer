// Publishing finished analysis results to the UI without blocking either side.
//
// A triple buffer. Three copies of T: the producer always writes into one
// nobody is reading and then atomically publishes it; the consumer atomically
// takes the most recent. Neither ever waits, so the analysis thread cannot be
// stalled by a slow redraw and the UI cannot observe a half-written frame.
//
// It also resolves a rate mismatch for free. Analysis produces a few hundred
// frames a second while the display runs at 60-120 Hz, so most frames are
// overwritten before anyone looks at them. That is the correct outcome: nobody
// can see a spectrum update that was never drawn.
//
// How it works
//
// The three buffers are owned by role, not by position. At any moment one is
// the producer's "input", one is the consumer's "output", and the third is the
// "back" buffer, whose index lives in a single atomic byte together with a
// "fresh" flag:
//
//   publish: swap(back, input | fresh)    - hand over what was just written,
//                                           take whatever was in the back
//   read:    if fresh, swap(back, output) - take the newest, return the old
//
// One atomic exchange per operation, never a loop or a lock. Header-only
// because it is a template.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <utility>

namespace analyzer::engine {

namespace detail {

template <class T>
struct TripleBuffer {
    explicit TripleBuffer(const T& initial) : slots{Slot{initial}, Slot{initial}, Slot{initial}} {}

    // Each copy on its own cache line, so the two threads writing different
    // buffers never contend for one.
    struct alignas(64) Slot {
        T value;
    };

    static constexpr std::uint8_t kIndexMask = 0b011;
    static constexpr std::uint8_t kFresh = 0b100;

    std::array<Slot, 3> slots;
    // Back buffer index, plus kFresh while it holds something unread.
    // Buffer 0 starts as the input, 1 as the back, 2 as the output.
    std::atomic<std::uint8_t> back{1};
};

}  // namespace detail

template <class T>
class SnapshotReader;

template <class T>
class SnapshotPublisher;

// Create a publisher/reader pair, with all three buffers initialised from
// `initial`.
//
// Allocation happens here, once, so publishing never does - which means T
// should be sized at creation (vectors reserved to their final length).
template <class T>
std::pair<SnapshotPublisher<T>, SnapshotReader<T>> snapshot_channel(const T& initial) {
    auto shared = std::make_shared<detail::TripleBuffer<T>>(initial);
    return {SnapshotPublisher<T>(shared), SnapshotReader<T>(shared)};
}

// Producer end. Lives on the analysis thread. Move-only: one producer.
template <class T>
class SnapshotPublisher {
public:
    SnapshotPublisher(SnapshotPublisher&&) noexcept = default;
    SnapshotPublisher& operator=(SnapshotPublisher&&) noexcept = default;
    SnapshotPublisher(const SnapshotPublisher&) = delete;
    SnapshotPublisher& operator=(const SnapshotPublisher&) = delete;

    // Mutate the pending buffer in place with `fill(T&)`, then publish it.
    //
    // In place is the point. Publishing by value would move or copy the whole
    // snapshot - and a snapshot holds vectors of bins, so a copy would allocate
    // on every frame.
    //
    // The buffer retains whatever was written two publishes ago, not the value
    // most recently published, so `fill` must overwrite every field it cares
    // about rather than assuming a starting state.
    template <class Fill>
    void publish_with(Fill&& fill) {
        fill(shared_->slots[input_].value);
        // acq_rel: release so the reader sees everything `fill` wrote; acquire
        // so we see everything the reader finished with in the buffer we take.
        const std::uint8_t previous = shared_->back.exchange(
            static_cast<std::uint8_t>(input_ | Shared::kFresh), std::memory_order_acq_rel);
        input_ = previous & Shared::kIndexMask;
    }

private:
    using Shared = detail::TripleBuffer<T>;
    friend std::pair<SnapshotPublisher<T>, SnapshotReader<T>> snapshot_channel<T>(const T&);
    explicit SnapshotPublisher(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

    std::shared_ptr<Shared> shared_;
    std::uint8_t input_ = 0;
};

// Consumer end. Lives on the UI thread. Move-only: one consumer.
template <class T>
class SnapshotReader {
public:
    SnapshotReader(SnapshotReader&&) noexcept = default;
    SnapshotReader& operator=(SnapshotReader&&) noexcept = default;
    SnapshotReader(const SnapshotReader&) = delete;
    SnapshotReader& operator=(const SnapshotReader&) = delete;

    // The most recently published value, or the previous one if nothing new
    // arrived. Never blocks. The reference is valid until the next read().
    const T& read() noexcept {
        if (has_update()) {
            const std::uint8_t previous =
                shared_->back.exchange(output_, std::memory_order_acq_rel);
            output_ = previous & Shared::kIndexMask;
        }
        return shared_->slots[output_].value;
    }

    // Whether a new value is waiting, without consuming it.
    //
    // Useful for skipping a redraw entirely when nothing changed - which is how
    // the idle-CPU target gets met, since a free-running timer redrawing
    // identical data is the real battery cost.
    bool has_update() const noexcept {
        return (shared_->back.load(std::memory_order_relaxed) & Shared::kFresh) != 0;
    }

private:
    using Shared = detail::TripleBuffer<T>;
    friend std::pair<SnapshotPublisher<T>, SnapshotReader<T>> snapshot_channel<T>(const T&);
    explicit SnapshotReader(std::shared_ptr<Shared> shared) : shared_(std::move(shared)) {}

    std::shared_ptr<Shared> shared_;
    std::uint8_t output_ = 2;
};

}  // namespace analyzer::engine
