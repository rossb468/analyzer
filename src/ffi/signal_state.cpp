#include "ffi/signal_state.hpp"

#include <cmath>

#include "ffi/internal.hpp"

namespace analyzer::ffi {

void SignalState::set_sweep(float start_hz, float end_hz, float seconds, float level_db) noexcept {
    const float amplitude = amplitude_from_db(level_db);
    // Relaxed stores, then one release increment: a reader that sees the new
    // generation is guaranteed to see every field written before it.
    kind_.store(AnalyzerSignal_Sweep, std::memory_order_relaxed);
    amplitude_bits_.store(bits(amplitude), std::memory_order_relaxed);
    hz_bits_.store(bits(std::fmax(start_hz, 0.0f)), std::memory_order_relaxed);
    end_hz_bits_.store(bits(std::fmax(end_hz, 0.0f)), std::memory_order_relaxed);
    seconds_bits_.store(bits(std::fmax(seconds, 0.0f)), std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
}

void SignalState::set(AnalyzerSignal signal, float level_db, float hz) noexcept {
    const float amplitude = amplitude_from_db(level_db);
    kind_.store(signal, std::memory_order_relaxed);
    amplitude_bits_.store(bits(amplitude), std::memory_order_relaxed);
    hz_bits_.store(bits(std::fmax(hz, 0.0f)), std::memory_order_relaxed);
    generation.fetch_add(1, std::memory_order_release);
}

dsp::Signal SignalState::signal() const noexcept {
    const float amplitude = value_of(amplitude_bits_.load(std::memory_order_relaxed));
    const float hz = value_of(hz_bits_.load(std::memory_order_relaxed));
    switch (kind_.load(std::memory_order_relaxed)) {
        case AnalyzerSignal_Sine: return dsp::Signal::sine(hz, amplitude);
        case AnalyzerSignal_WhiteNoise: return dsp::Signal::white_noise(amplitude);
        case AnalyzerSignal_PinkNoise: return dsp::Signal::pink_noise(amplitude);
        case AnalyzerSignal_Sweep:
            // One pass. A repeating sweep would overlap its own tail.
            return dsp::Signal::sweep(hz, value_of(end_hz_bits_.load(std::memory_order_relaxed)),
                                      value_of(seconds_bits_.load(std::memory_order_relaxed)),
                                      amplitude, /*repeat=*/false);
        case AnalyzerSignal_Silence: break;
    }
    return dsp::Signal::silence();
}

}  // namespace analyzer::ffi
