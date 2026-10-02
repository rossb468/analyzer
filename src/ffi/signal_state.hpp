// Generator settings shared with the audio callback.
//
// Atomics rather than a lock: the callback reads these on the real-time
// thread, where blocking on a UI thread's mutex is the classic way to produce
// a dropout. `generation` is bumped last, so seeing a new value guarantees the
// fields behind it are already written.

#pragma once

#include <atomic>
#include <bit>
#include <cstdint>

#include "dsp/generator.hpp"
#include "ffi/c_api.hpp"

namespace analyzer::ffi {

class SignalState {
public:
    SignalState() noexcept = default;

    SignalState(AnalyzerSignal signal, float level_db, float hz) noexcept {
        set(signal, level_db, hz);
    }

    // Arm a one-pass sweep.
    //
    // Separate from set() because a sweep carries two more parameters, and
    // because it must not repeat: deconvolution needs the recorded response to
    // contain exactly one pass of the stimulus it is divided by.
    void set_sweep(float start_hz, float end_hz, float seconds, float level_db) noexcept;

    // Choose a steady stimulus. A level above 0 dBFS is capped there.
    void set(AnalyzerSignal signal, float level_db, float hz) noexcept;

    // What the generator should be producing now. Read on the audio thread.
    dsp::Signal signal() const noexcept;

    // Incremented, with release ordering, after every change. The callback only
    // reloads the signal when this moves.
    std::atomic<std::uint32_t> generation{0};

private:
    static std::uint32_t bits(float value) noexcept { return std::bit_cast<std::uint32_t>(value); }
    static float value_of(std::uint32_t bits) noexcept { return std::bit_cast<float>(bits); }

    std::atomic<std::uint32_t> kind_{0};
    std::atomic<std::uint32_t> amplitude_bits_{0};
    std::atomic<std::uint32_t> hz_bits_{0};
    // Upper end of a sweep. Unused by every other signal.
    std::atomic<std::uint32_t> end_hz_bits_{0};
    // Sweep duration in seconds. Unused by every other signal.
    std::atomic<std::uint32_t> seconds_bits_{0};
};

}  // namespace analyzer::ffi
