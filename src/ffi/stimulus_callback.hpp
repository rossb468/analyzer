// The audio callback a session installs.
//
// It does the whole of what the callback is allowed to do, in this order:
// generate the stimulus, write the output, write into the ring, return. No
// allocation, no locks, no logging, no syscalls; everything it touches is sized
// when the session starts. engine::rt_section arms the allocation trap around
// the body, so a violation aborts in debug and test builds instead of showing
// up as a dropout on somebody's hardware.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "audio/stream.hpp"
#include "dsp/generator.hpp"
#include "engine/ring.hpp"
#include "ffi/eq_processor.hpp"
#include "ffi/signal_state.hpp"

namespace analyzer::ffi {

class StimulusCallback final : public audio::AudioCallback {
public:
    // `channels` is what the ring carries per frame: the device's inputs, plus
    // one when the transfer function's reference is the generator's own samples
    // (`internal_reference`). `playing` is whether an output stream was opened;
    // a session started silent stays silent, since opening one is a device
    // operation and cannot happen later.
    StimulusCallback(engine::CaptureSink sink, std::shared_ptr<SignalState> signal,
                     EqProcessor equaliser, float sample_rate, std::size_t channels, bool playing,
                     bool internal_reference);

    void process(audio::AudioBuffers& buffers) noexcept override;

private:
    engine::CaptureSink sink_;
    std::shared_ptr<SignalState> signal_;
    EqProcessor equaliser_;
    dsp::Generator generator_;
    std::size_t channels_;
    bool playing_;
    bool internal_reference_;
    std::uint32_t generation_;
    std::vector<float> stimulus_;
    // Only allocated when the reference has to be spliced in; the common
    // spectrum path writes the device's own buffer straight into the ring.
    std::vector<float> block_;
};

}  // namespace analyzer::ffi
