// A fixed-capacity delay line, used to hold the reference channel back.
//
// In a transfer-function measurement the reference arrives first - sound takes
// time to travel from the loudspeaker to the microphone - so the reference is
// delayed until both channels describe the same instant. Left uncompensated,
// the phase curve winds through hundreds of turns and coherence collapses well
// before 1 kHz.
//
// Preallocated at construction so applying a delay never touches the
// allocator, and circular so changing the delay costs nothing.

#pragma once

#include <algorithm>
#include <cstddef>
#include <span>
#include <vector>

namespace analyzer::engine {

class DelayLine {
public:
    // A line able to delay by up to `max_delay` samples.
    explicit DelayLine(std::size_t max_delay) : buffer_(max_delay + 1, 0.0f) {}

    // The longest delay this line can hold.
    std::size_t capacity() const noexcept { return buffer_.size() - 1; }

    // Write `input` and read it back `delay` samples later into `output`.
    //
    // Processes min(input.size(), output.size()) samples. A delay longer than
    // capacity() is clamped rather than wrapping into nonsense. State carries
    // across calls, since a block boundary falls wherever the device chooses
    // and carries no meaning.
    void process(std::span<const float> input, std::span<float> output,
                 std::size_t delay) noexcept {
        delay = std::min(delay, capacity());
        const std::size_t length = buffer_.size();
        const std::size_t count = std::min(input.size(), output.size());
        for (std::size_t i = 0; i < count; ++i) {
            buffer_[write_] = input[i];
            write_ = (write_ + 1) % length;
            output[i] = buffer_[(write_ + length - delay - 1) % length];
        }
    }

private:
    std::vector<float> buffer_;
    std::size_t write_ = 0;
};

}  // namespace analyzer::engine
