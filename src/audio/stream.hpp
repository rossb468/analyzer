// Stream configuration, the real-time callback contract, and stream control.
//
// Three things live here because they are used together at the moment a stream
// opens: what to open (StreamConfig), the contract between the hardware's
// thread and the code that consumes its audio (AudioBuffers and
// AudioCallback), and the handle that starts and stops it (AudioStream).

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "audio/device.hpp"

namespace analyzer::audio {

// What to open.
//
// Input and output are named separately because measurement rigs routinely use
// two different devices, and on macOS that means an aggregate device. Neither
// side is required, so this also covers analyse-only and generate-only setups.
struct StreamConfig {
    // Capture device, if capturing.
    std::optional<DeviceId> input;
    // Playback device, if playing.
    std::optional<DeviceId> output;
    // Requested rate in hertz.
    double sample_rate = 0.0;
    // Requested callback size in frames. Backends may round this.
    std::uint32_t buffer_frames = 0;
    // Device channel indices to capture, in the order they should be delivered.
    std::vector<std::uint32_t> input_channels;
    // Device channel indices to play to, in the order buffers supply them.
    std::vector<std::uint32_t> output_channels;

    // Basic self-consistency check, before a backend touches the hardware.
    //
    // Throws NothingToDoError if neither direction has channels, and
    // UnsupportedSampleRateError for a non-positive rate.
    void validate() const;

    friend bool operator==(const StreamConfig&, const StreamConfig&) = default;
};

// Where the hardware's latency actually sits.
//
// This exists for the transfer-function reference problem. Measuring a system
// needs to know what was sent as well as what was heard; the accurate way is a
// physical loopback cable, but the convenient way is to use the generated
// buffer as the reference and compensate for the round trip. That is only
// possible if the backend reports these honestly, and the numbers are
// approximate on every platform - hence a one-time user calibration on top.
struct StreamLatency {
    // Frames of delay between sound arriving and the callback seeing it.
    std::uint32_t input_frames = 0;
    // Frames of delay between the callback writing and sound emerging.
    std::uint32_t output_frames = 0;
    // Additional device safety offset the driver imposes.
    std::uint32_t safety_offset_frames = 0;

    // Total output-to-input delay in frames. Saturates rather than wrapping.
    std::uint32_t round_trip_frames() const noexcept;

    // Total output-to-input delay in seconds at `sample_rate`. Zero for a
    // non-positive rate, rather than a division by zero.
    double round_trip_seconds(double sample_rate) const noexcept;

    friend bool operator==(const StreamLatency&, const StreamLatency&) = default;
};

// One channel of an interleaved buffer, seen as a sequence of samples.
//
// A view, not a copy: it points into the buffer it came from and is valid for
// as long as that buffer is, which is one callback. Reading it never allocates,
// so it is safe on the audio thread. Iterating visits `frames` samples, one per
// frame, stepping over the other channels.
class ChannelView {
public:
    class Iterator {
    public:
        using iterator_category = std::forward_iterator_tag;
        using value_type = float;
        using difference_type = std::ptrdiff_t;
        using pointer = const float*;
        using reference = const float&;

        Iterator() = default;

        reference operator*() const noexcept { return base_[index_ * stride_]; }
        Iterator& operator++() noexcept {
            ++index_;
            return *this;
        }
        Iterator operator++(int) noexcept {
            Iterator before = *this;
            ++index_;
            return before;
        }

        friend bool operator==(const Iterator&, const Iterator&) = default;

    private:
        friend class ChannelView;
        Iterator(const float* base, std::size_t stride, std::size_t index) noexcept
            : base_(base), stride_(stride), index_(index) {}

        // Frame indices rather than a running pointer: a pointer stepped by
        // the stride would end up to stride-1 elements past the end of the
        // buffer, which is not a pointer the language lets you form.
        const float* base_ = nullptr;
        std::size_t stride_ = 1;
        std::size_t index_ = 0;
    };

    // An empty view: what an out-of-range channel yields.
    ChannelView() noexcept = default;

    Iterator begin() const noexcept { return Iterator(base_, stride_, 0); }
    Iterator end() const noexcept { return Iterator(base_, stride_, size_); }

    // Samples in the view: one per frame, or zero for an empty view.
    std::size_t size() const noexcept { return size_; }
    bool empty() const noexcept { return size_ == 0; }

    // Sample `frame`. Must be less than size().
    float operator[](std::size_t frame) const noexcept;

private:
    friend class AudioBuffers;
    ChannelView(const float* base, std::size_t stride, std::size_t size) noexcept
        : base_(base), stride_(stride), size_(size) {}

    const float* base_ = nullptr;
    std::size_t stride_ = 1;
    std::size_t size_ = 0;
};

// Interleaved buffers for one callback invocation.
//
// Both sides are interleaved as the hardware presents them. Deinterleaving is
// the engine's job, not the backend's, so there is exactly one place that
// decides the memory layout the analysis chain sees.
//
// A non-owning view of the backend's preallocated scratch: it holds spans, so
// building one on the audio thread allocates nothing, and it must not outlive
// the callback it was made for.
class AudioBuffers {
public:
    // Wrap a backend's buffers.
    //
    // Each span's length must equal frames * its channel count. A backend
    // getting this wrong is a bug that must not be papered over on the
    // real-time path, so a mismatch aborts rather than being corrected.
    AudioBuffers(std::span<const float> input, std::span<float> output, std::size_t input_channels,
                 std::size_t output_channels, std::size_t frames) noexcept;

    // Frames in this callback.
    std::size_t frames() const noexcept { return frames_; }

    // Number of interleaved capture channels.
    std::size_t input_channels() const noexcept { return input_channels_; }

    // Number of interleaved playback channels.
    std::size_t output_channels() const noexcept { return output_channels_; }

    // The interleaved capture buffer.
    std::span<const float> input() const noexcept { return input_; }

    // The interleaved playback buffer, to be written.
    std::span<float> output() noexcept { return output_; }

    // Samples of one capture channel, in order.
    //
    // Yields an empty view if `channel` is out of range, rather than aborting:
    // this runs on the real-time thread, where a crash is worse than silence.
    ChannelView input_channel(std::size_t channel) const noexcept;

    // Write silence to the whole playback buffer.
    //
    // Backends do not guarantee the buffer arrives zeroed, and stale contents
    // played back at full scale is the loudest possible bug.
    void silence_output() noexcept;

private:
    std::span<const float> input_;
    std::span<float> output_;
    std::size_t input_channels_;
    std::size_t output_channels_;
    std::size_t frames_;
};

// The real-time audio callback.
//
// Real-time contract
//
// process() runs on a thread with a hard deadline - typically 2.67 ms at 128
// frames and 48 kHz. Returning late means the hardware plays whatever was in
// the buffer, which is an audible click, and for a measurement tool it silently
// corrupts the data being collected.
//
// So process() must not:
//
// - allocate or free memory (unbounded worst case),
// - take a lock (a lower-priority thread holding it inverts priority),
// - log, touch the filesystem, or make any syscall,
// - do anything that can page-fault or block.
//
// In practice it should deinterleave into a lock-free queue and return. The
// expensive work belongs on an analysis thread reading the other end.
//
// This is enforced rather than trusted: debug and test builds run the callback
// inside an allocation trap that aborts on violation. It is also noexcept, and
// an override must be: an exception cannot unwind through the C code of a
// platform audio API, so one that tried would terminate the process.
//
// Threads
//
// A callback is built on the setup thread, where allocating is fine, handed to
// a backend, and from then on invoked only by the backend's audio thread until
// the stream is destroyed. It needs no locking of its own for state only it
// touches.
class AudioCallback {
public:
    virtual ~AudioCallback() = default;

    // Consume buffers.input() and fill buffers.output().
    virtual void process(AudioBuffers& buffers) noexcept = 0;

protected:
    AudioCallback() = default;
    AudioCallback(const AudioCallback&) = default;
    AudioCallback& operator=(const AudioCallback&) = default;
};

namespace detail {

// Adapts any callable to AudioCallback. See make_callback().
template <class F>
class FunctionCallback final : public AudioCallback {
public:
    explicit FunctionCallback(F function) : function_(std::move(function)) {}

    // The call is direct and the callable is stored by value, so invoking this
    // allocates nothing - unlike a std::function, which may. Whatever the
    // callable itself does is its own business and subject to the same
    // contract.
    void process(AudioBuffers& buffers) noexcept override { function_(buffers); }

private:
    F function_;
};

}  // namespace detail

// Wrap a lambda (or any callable taking AudioBuffers&) as an AudioCallback.
//
//   auto callback = make_callback([](AudioBuffers& buffers) { buffers.silence_output(); });
//
// The callable is copied or moved into the returned object, which is allocated
// here, at setup time; invoking it afterwards does not allocate. A lambda that
// mutates its own state must be declared `mutable`.
template <class F>
std::unique_ptr<AudioCallback> make_callback(F&& function) {
    using Function = std::decay_t<F>;
    static_assert(std::is_invocable_v<Function&, AudioBuffers&>,
                  "a callback must be callable with an AudioBuffers&");
    return std::make_unique<detail::FunctionCallback<Function>>(std::forward<F>(function));
}

// A configured, controllable stream.
//
// Threads: a stream is controlled - started, stopped, destroyed - by one thread
// at a time, usually the one that opened it. Its callback runs on the
// backend's audio thread, which never touches the control state.
class AudioStream {
public:
    virtual ~AudioStream() = default;

    // Begin calling the callback.
    //
    // Throws AlreadyRunningError if already started, or BackendError if the
    // hardware refuses.
    virtual void start() = 0;

    // Stop calling the callback. Idempotent.
    //
    // Throws BackendError if the platform fails to stop the stream.
    virtual void stop() = 0;

    // Whether the callback is currently being invoked.
    virtual bool is_running() const noexcept = 0;

    // What the backend actually granted.
    //
    // Rarely identical to what was requested: buffer sizes get rounded and
    // rates get substituted, and everything downstream must use these values
    // rather than the requested ones.
    virtual const StreamConfig& config() const noexcept = 0;

    // Hardware latency, for reference-signal compensation.
    virtual StreamLatency latency() const noexcept = 0;

protected:
    // Protected so a stream can be copied or moved as its derived type but
    // never sliced through the base.
    AudioStream() = default;
    AudioStream(const AudioStream&) = default;
    AudioStream(AudioStream&&) = default;
    AudioStream& operator=(const AudioStream&) = default;
    AudioStream& operator=(AudioStream&&) = default;
};

}  // namespace analyzer::audio
