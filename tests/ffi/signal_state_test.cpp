// Ported from crates/analyzer-ffi/src/lib.rs.

#include <atomic>
#include <cmath>

#include <gtest/gtest.h>

#include "ffi/signal_state.hpp"

namespace analyzer::ffi {
namespace {

// A level above full scale cannot be produced and would only clip.
TEST(SignalState, TheStimulusLevelIsCappedAtFullScale) {
    const SignalState state(AnalyzerSignal_Sine, 40.0f, 1000.0f);
    const dsp::Signal signal = state.signal();
    ASSERT_EQ(signal.kind, dsp::Signal::Kind::Sine) << "wrong signal";
    EXPECT_NEAR(signal.amplitude, 1.0f, 1e-6f) << "expected clamping to unity";
}

// Decibels must reach the generator as a linear amplitude.
TEST(SignalState, TheStimulusLevelConvertsFromDecibels) {
    const SignalState state(AnalyzerSignal_PinkNoise, -20.0f, 0.0f);
    const dsp::Signal signal = state.signal();
    ASSERT_EQ(signal.kind, dsp::Signal::Kind::PinkNoise) << "wrong signal";
    EXPECT_NEAR(signal.amplitude, 0.1f, 1e-6f) << "-20 dB is 0.1";
}

// Every change must be visible to the audio thread, which only reloads when the
// counter moves.
TEST(SignalState, ChangingTheStimulusBumpsTheGeneration) {
    SignalState state(AnalyzerSignal_Silence, -20.0f, 1000.0f);
    const std::uint32_t before = state.generation.load(std::memory_order_acquire);
    state.set(AnalyzerSignal_Sine, -6.0f, 440.0f);
    EXPECT_GT(state.generation.load(std::memory_order_acquire), before);
    const dsp::Signal signal = state.signal();
    EXPECT_EQ(signal.kind, dsp::Signal::Kind::Sine);
    EXPECT_NEAR(signal.hz, 440.0f, 1e-6f);
}

// A sweep must be one pass. A repeating one would overlap its own tail and
// deconvolve into an impulse response with a second arrival in it.
TEST(SignalState, AnArmedSweepDoesNotRepeat) {
    SignalState state;
    state.set_sweep(20.0f, 20'000.0f, 2.0f, -12.0f);
    const dsp::Signal signal = state.signal();
    ASSERT_EQ(signal.kind, dsp::Signal::Kind::Sweep) << "expected a sweep";
    EXPECT_NEAR(signal.start_hz, 20.0f, 1e-3f);
    EXPECT_NEAR(signal.end_hz, 20'000.0f, 1e-3f);
    EXPECT_NEAR(signal.seconds, 2.0f, 1e-6f);
    EXPECT_FALSE(signal.repeat) << "a measurement sweep must be a single pass";
}

// A fresh state is silent, and a stored kind that means nothing reads as silence
// rather than as whatever happens to follow it.
TEST(SignalState, ADefaultStateIsSilent) {
    const SignalState state;
    EXPECT_EQ(state.signal().kind, dsp::Signal::Kind::Silence);
    EXPECT_EQ(state.generation.load(), 0u);

    SignalState odd;
    odd.set(static_cast<AnalyzerSignal>(99), -6.0f, 1000.0f);
    EXPECT_EQ(odd.signal().kind, dsp::Signal::Kind::Silence);
}

// The frequency is never negative, whatever the caller sent.
TEST(SignalState, ANegativeFrequencyIsFlooredAtZero) {
    SignalState state;
    state.set(AnalyzerSignal_Sine, -6.0f, -50.0f);
    EXPECT_EQ(state.signal().hz, 0.0f);
}

}  // namespace
}  // namespace analyzer::ffi
