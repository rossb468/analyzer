#include "audio/stream.hpp"

#include <algorithm>
#include <limits>

#include "audio/error.hpp"
#include "base/contract.hpp"

namespace analyzer::audio {

void StreamConfig::validate() const {
    if (input_channels.empty() && output_channels.empty()) {
        throw NothingToDoError();
    }
    if (sample_rate <= 0.0) {
        throw UnsupportedSampleRateError("stream", sample_rate);
    }
}

std::uint32_t StreamLatency::round_trip_frames() const noexcept {
    // Summed in 64 bits so that the clamp is the only place saturation happens.
    const std::uint64_t total = std::uint64_t{input_frames} + output_frames + safety_offset_frames;
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(total, std::numeric_limits<std::uint32_t>::max()));
}

double StreamLatency::round_trip_seconds(double sample_rate) const noexcept {
    if (sample_rate <= 0.0) {
        return 0.0;
    }
    return static_cast<double>(round_trip_frames()) / sample_rate;
}

float ChannelView::operator[](std::size_t frame) const noexcept {
    ANALYZER_EXPECTS(frame < size_, "frame index out of range");
    return base_[frame * stride_];
}

AudioBuffers::AudioBuffers(std::span<const float> input, std::span<float> output,
                           std::size_t input_channels, std::size_t output_channels,
                           std::size_t frames) noexcept
    : input_(input),
      output_(output),
      input_channels_(input_channels),
      output_channels_(output_channels),
      frames_(frames) {
    ANALYZER_EXPECTS(input.size() == frames * input_channels,
                     "input buffer must be frames * input_channels");
    ANALYZER_EXPECTS(output.size() == frames * output_channels,
                     "output buffer must be frames * output_channels");
}

ChannelView AudioBuffers::input_channel(std::size_t channel) const noexcept {
    // Also covers a zero-frame buffer, whose data() may be null and so cannot
    // be offset by `channel`.
    if (channel >= input_channels_ || frames_ == 0) {
        return {};
    }
    return {input_.data() + channel, input_channels_, frames_};
}

void AudioBuffers::silence_output() noexcept {
    std::ranges::fill(output_, 0.0f);
}

}  // namespace analyzer::audio
