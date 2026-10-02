#include "ffi/stimulus_callback.hpp"

#include <algorithm>
#include <span>
#include <utility>

#include "engine/rt.hpp"
#include "ffi/internal.hpp"

namespace analyzer::ffi {

StimulusCallback::StimulusCallback(engine::CaptureSink sink, std::shared_ptr<SignalState> signal,
                                   EqProcessor equaliser, float sample_rate, std::size_t channels,
                                   bool playing, bool internal_reference)
    : sink_(std::move(sink)),
      signal_(std::move(signal)),
      equaliser_(std::move(equaliser)),
      generator_(sample_rate, signal_->signal(), dsp::kDefaultSeed),
      channels_(channels),
      playing_(playing),
      internal_reference_(internal_reference),
      generation_(signal_->generation.load(std::memory_order_acquire)),
      stimulus_(kMaxCallbackFrames, 0.0f),
      block_(internal_reference ? kMaxCallbackFrames * channels : 0, 0.0f) {}

void StimulusCallback::process(audio::AudioBuffers& buffers) noexcept {
    engine::rt_section([&] {
        // Clamped, not trusted. A device is free to hand over a larger block
        // than it promised, and growing a buffer on this thread is exactly what
        // must never happen.
        const std::size_t frames = std::min(buffers.frames(), kMaxCallbackFrames);

        if (signal_->generation.load(std::memory_order_acquire) != generation_) {
            generation_ = signal_->generation.load(std::memory_order_acquire);
            generator_.set_signal(signal_->signal());
        }

        const std::span<float> stimulus(stimulus_.data(), frames);
        if (playing_) {
            generator_.fill(stimulus);
            // Equalise before anything sees it, so the reference channel
            // carries what was actually played. Filtering only the output would
            // make the transfer function report the equaliser's own curve as if
            // it were the room's.
            equaliser_.process(stimulus);
        } else {
            std::fill(stimulus.begin(), stimulus.end(), 0.0f);
        }

        // Silence first so an oversized callback leaves no stale audio in the
        // tail rather than playing it back.
        buffers.silence_output();
        const std::size_t outputs = buffers.output_channels();
        if (outputs > 0) {
            const std::span<float> output = buffers.output();
            const std::size_t output_frames = std::min(output.size() / outputs, stimulus.size());
            for (std::size_t frame = 0; frame < output_frames; ++frame) {
                // The same mono stimulus to every selected channel: a transfer
                // function measures one path, and decorrelated noise between
                // channels would make the room sum unpredictably.
                std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(frame * outputs), outputs,
                            stimulus[frame]);
            }
        }

        if (internal_reference_) {
            // The stimulus is appended to the device's own channels as one more,
            // so it travels through the same ring, in the same block, as the
            // audio it will be compared against. Nothing downstream can then
            // slide the two apart.
            const std::span<const float> input = buffers.input();
            const std::size_t stride = std::max<std::size_t>(buffers.input_channels(), 1);
            const std::size_t spliced = std::min({frames, input.size() / stride});
            for (std::size_t frame = 0; frame < spliced; ++frame) {
                float* out = block_.data() + frame * channels_;
                const float* source = input.data() + frame * stride;
                std::copy_n(source, std::min(channels_, stride), out);
                if (stride < channels_) {
                    out[stride] = stimulus[frame];
                }
            }
            sink_.write_interleaved(std::span<const float>(block_.data(), frames * channels_));
        } else {
            sink_.write_interleaved(buffers.input());
        }
    });
}

}  // namespace analyzer::ffi
