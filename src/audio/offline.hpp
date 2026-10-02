// A backend that reads from memory instead of hardware.
//
// Two jobs. It drives the headless harness, which is how numerical correctness
// gets proven before any UI exists. And it forces AudioBackend to have a
// working implementation from the start, so the interface is shaped by
// something real rather than by guesswork about what CoreAudio will want.
//
// It is explicitly **not** real-time: OfflineStream::run_to_end() runs the
// callback as fast as it can, on the calling thread, and grows a vector to hold
// what the callback wrote. That makes it useful for tests and useless for
// measuring latency.

#pragma once

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "audio/backend.hpp"
#include "audio/device.hpp"
#include "audio/stream.hpp"

namespace analyzer::audio {

// Device id the offline backend reports.
inline constexpr std::string_view kOfflineDeviceId = "offline";

// Interleaved sample material to feed a stream.
struct Source {
    // Interleaved samples, frames * channels long.
    std::vector<float> samples;
    // Channels the interleaving assumes.
    std::size_t channels;
    // Nominal rate, reported through the device info.
    double sample_rate;

    // Build a source from interleaved samples.
    //
    // `channels` must be non-zero and the sample count a whole number of
    // frames.
    Source(std::vector<float> samples, std::size_t channels, double sample_rate);

    // Build a single-channel source.
    static Source mono(std::vector<float> samples, double sample_rate);

    // Frames of material available.
    std::size_t frames() const noexcept { return samples.size() / channels; }

    friend bool operator==(const Source&, const Source&) = default;
};

class OfflineStream;

// AudioBackend over an in-memory Source.
class OfflineBackend final : public AudioBackend {
public:
    // Wrap `source`, delivering `block_frames` per callback. `block_frames`
    // must be non-zero.
    OfflineBackend(Source source, std::size_t block_frames);

    // Open a concrete OfflineStream.
    //
    // Prefer this over open() whenever the offline controls are needed. open()
    // returns std::unique_ptr<AudioStream>, which erases OfflineStream::pump(),
    // run_to_end() and captured_output() - and a hardware stream has no
    // equivalent of those to justify putting them on the interface, since the
    // OS drives it rather than the caller.
    //
    // Throws ChannelOutOfRangeError for a channel the synthetic device does not
    // have, or whatever StreamConfig::validate() rejects.
    OfflineStream open_offline(const StreamConfig& config, std::unique_ptr<AudioCallback> callback);

    std::string_view name() const noexcept override { return "offline"; }
    std::vector<DeviceInfo> devices() const override;
    std::unique_ptr<AudioStream> open(const StreamConfig& config,
                                      std::unique_ptr<AudioCallback> callback) override;

private:
    DeviceInfo device() const;

    Source source_;
    std::size_t block_frames_;
};

// A stream that pulls from memory. Not real-time.
//
// Driven by the caller: nothing runs until pump() or run_to_end() is called,
// and the callback runs on the calling thread. start() and stop() only flip the
// flag is_running() reports.
class OfflineStream final : public AudioStream {
public:
    // Run one block through the callback, returning frames processed.
    //
    // Returns zero once the source is exhausted.
    std::size_t pump();

    // Run the whole source through the callback, returning total frames.
    std::size_t run_to_end();

    // Everything the callback wrote, interleaved across the selected output
    // channels.
    std::span<const float> captured_output() const noexcept { return captured_; }

    // Frames consumed so far.
    std::size_t position() const noexcept { return position_; }

    // Rewind and discard captured output.
    void rewind() noexcept;

    void start() override;
    void stop() override;
    bool is_running() const noexcept override { return running_; }
    const StreamConfig& config() const noexcept override { return config_; }
    StreamLatency latency() const noexcept override;

private:
    friend class OfflineBackend;
    OfflineStream(StreamConfig config, std::unique_ptr<AudioCallback> callback, Source source,
                  std::size_t block);

    StreamConfig config_;
    std::unique_ptr<AudioCallback> callback_;
    Source source_;
    std::size_t position_ = 0;
    std::size_t block_;
    // Sized once, at open, for a full block; pump() only slices them.
    std::vector<float> input_scratch_;
    std::vector<float> output_scratch_;
    std::vector<float> captured_;
    bool running_ = false;
};

}  // namespace analyzer::audio
