#include "audio/offline.hpp"

#include <algorithm>
#include <utility>

#include "audio/error.hpp"
#include "base/contract.hpp"

namespace analyzer::audio {

Source::Source(std::vector<float> samples_in, std::size_t channels_in, double sample_rate_in)
    : samples(std::move(samples_in)), channels(channels_in), sample_rate(sample_rate_in) {
    ANALYZER_EXPECTS(channels > 0, "source must have at least one channel");
    ANALYZER_EXPECTS(samples.size() % channels == 0,
                     "sample count must be a whole number of frames");
}

Source Source::mono(std::vector<float> samples, double sample_rate) {
    return Source(std::move(samples), 1, sample_rate);
}

OfflineBackend::OfflineBackend(Source source, std::size_t block_frames)
    : source_(std::move(source)), block_frames_(block_frames) {
    ANALYZER_EXPECTS(block_frames_ > 0, "block size must be non-zero");
}

OfflineStream OfflineBackend::open_offline(const StreamConfig& config,
                                           std::unique_ptr<AudioCallback> callback) {
    config.validate();

    const DeviceInfo device_info = device();
    for (const std::uint32_t channel : config.input_channels) {
        if (channel >= device_info.input_channels) {
            throw ChannelOutOfRangeError(device_info.name, channel, device_info.input_channels);
        }
    }
    for (const std::uint32_t channel : config.output_channels) {
        if (channel >= device_info.output_channels) {
            throw ChannelOutOfRangeError(device_info.name, channel, device_info.output_channels);
        }
    }

    return OfflineStream(config, std::move(callback), source_, block_frames_);
}

DeviceInfo OfflineBackend::device() const {
    return DeviceInfo{
        .id = DeviceId(kOfflineDeviceId),
        .name = "Offline (in-memory)",
        .input_channels = static_cast<std::uint32_t>(source_.channels),
        // Enough to exercise multi-channel routing without pretending to be
        // a real interface.
        .output_channels = 2,
        .default_sample_rate = source_.sample_rate,
        .supported_sample_rates = {source_.sample_rate},
        .is_default_input = true,
        .is_default_output = true,
    };
}

std::vector<DeviceInfo> OfflineBackend::devices() const {
    return {device()};
}

std::unique_ptr<AudioStream> OfflineBackend::open(const StreamConfig& config,
                                                  std::unique_ptr<AudioCallback> callback) {
    return std::make_unique<OfflineStream>(open_offline(config, std::move(callback)));
}

OfflineStream::OfflineStream(StreamConfig config, std::unique_ptr<AudioCallback> callback,
                             Source source, std::size_t block)
    : config_(std::move(config)),
      callback_(std::move(callback)),
      source_(std::move(source)),
      block_(block),
      input_scratch_(block * config_.input_channels.size(), 0.0f),
      output_scratch_(block * config_.output_channels.size(), 0.0f) {}

std::size_t OfflineStream::pump() {
    const std::size_t total = source_.frames();
    const std::size_t remaining = total > position_ ? total - position_ : 0;
    if (remaining == 0) {
        return 0;
    }
    const std::size_t frames = std::min(block_, remaining);

    const std::size_t inputs = config_.input_channels.size();
    const std::size_t outputs = config_.output_channels.size();

    // Gather only the selected channels, in the order requested.
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const std::size_t src_base = (position_ + frame) * source_.channels;
        for (std::size_t slot = 0; slot < inputs; ++slot) {
            input_scratch_[frame * inputs + slot] =
                source_.samples[src_base + config_.input_channels[slot]];
        }
    }

    // Never hand the callback stale output contents.
    const std::span<float> output(output_scratch_.data(), frames * outputs);
    std::ranges::fill(output, 0.0f);

    AudioBuffers buffers(std::span<const float>(input_scratch_.data(), frames * inputs), output,
                         inputs, outputs, frames);
    callback_->process(buffers);

    captured_.insert(captured_.end(), output.begin(), output.end());
    position_ += frames;
    return frames;
}

std::size_t OfflineStream::run_to_end() {
    std::size_t total = 0;
    while (true) {
        const std::size_t frames = pump();
        if (frames == 0) {
            return total;
        }
        total += frames;
    }
}

void OfflineStream::rewind() noexcept {
    position_ = 0;
    captured_.clear();
}

void OfflineStream::start() {
    if (running_) {
        throw AlreadyRunningError();
    }
    running_ = true;
}

void OfflineStream::stop() {
    running_ = false;
}

StreamLatency OfflineStream::latency() const noexcept {
    // Nothing physical is involved, so there is genuinely no latency to
    // report. Claiming a plausible-looking number would be worse than zero.
    return {};
}

}  // namespace analyzer::audio
