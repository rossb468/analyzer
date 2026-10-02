// Ported from crates/analyzer-dsp/src/delay.rs.

#include "dsp/delay.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48000.0f;
constexpr std::size_t kSize = 8192;

// The Rust tests build their noise with dsp::Generator, which is ported
// separately. This is the same xorshift generator and Kellet pink filter, so a
// seed gives the same signal; swap it for Generator once it exists.
//
// Paul Kellet's refined pink filter: white noise shaped to -3 dB per octave.
std::vector<float> noise(std::size_t samples, std::uint64_t seed) {
    constexpr float kAmplitude = 0.5f;
    std::uint64_t state = seed == 0 ? 0x9E3779B97F4A7C15ull : seed;
    std::array<float, 7> b{};

    std::vector<float> out(samples);
    for (float& sample : out) {
        state ^= state >> 12;
        state ^= state << 25;
        state ^= state >> 27;
        const std::uint64_t scrambled = state * 0x2545F4914F6CDD1Dull;
        // Top 24 bits, mapped to [-1, 1); the low bits of xorshift are weak.
        const float white = static_cast<float>(scrambled >> 40) / 8388608.0f - 1.0f;

        b[0] = 0.99886f * b[0] + white * 0.0555179f;
        b[1] = 0.99332f * b[1] + white * 0.0750759f;
        b[2] = 0.96900f * b[2] + white * 0.153852f;
        b[3] = 0.86650f * b[3] + white * 0.3104856f;
        b[4] = 0.55000f * b[4] + white * 0.5329522f;
        b[5] = -0.7616f * b[5] - white * 0.0168980f;
        const float pink = b[0] + b[1] + b[2] + b[3] + b[4] + b[5] + b[6] + white * 0.5362f;
        b[6] = white * 0.115926f;

        sample = std::clamp(pink * 0.125f * kAmplitude, -1.0f, 1.0f);
    }
    return out;
}

// `source` arriving `delay` samples late, the head filled with silence rather
// than wrapped round from the tail: a rotation would be periodic, and a
// periodic signal cannot tell a delay from that delay minus the period.
std::vector<float> delayed(std::span<const float> source, std::size_t delay) {
    std::vector<float> out(source.size(), 0.0f);
    if (delay < source.size()) {
        std::copy(source.begin(), source.end() - static_cast<std::ptrdiff_t>(delay),
                  out.begin() + static_cast<std::ptrdiff_t>(delay));
    }
    return out;
}

TEST(DelayFinder, AnIntegerDelayIsRecoveredExactly) {
    const auto reference = noise(kSize, 1);
    for (const std::size_t delay :
         {std::size_t{1}, std::size_t{7}, std::size_t{64}, std::size_t{512}, std::size_t{2000}}) {
        const auto measurement = delayed(reference, delay);
        DelayFinder finder(kRate, kSize, Weighting::Phat);
        const auto estimate = finder.find(reference, measurement);
        ASSERT_TRUE(estimate.has_value());
        EXPECT_LT(std::abs(estimate->samples - static_cast<float>(delay)), 0.1f)
            << "delay " << delay << " came back as " << estimate->samples;
    }
}

TEST(DelayFinder, ZeroDelayIsFound) {
    const auto reference = noise(kSize, 2);
    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto estimate = finder.find(reference, reference);
    ASSERT_TRUE(estimate.has_value());
    EXPECT_LT(std::abs(estimate->samples), 0.1f) << "got " << estimate->samples;
}

// The measurement arriving *before* the reference is a real case - a
// mis-patched loopback, or an internal reference taken after the output
// buffer. The circular correlation must report it as negative, not as a
// huge positive delay.
TEST(DelayFinder, ANegativeDelayIsReportedAsNegative) {
    const auto source = noise(kSize, 3);
    const auto reference = delayed(source, 300);
    const auto& measurement = source;

    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto estimate = finder.find(reference, measurement);
    ASSERT_TRUE(estimate.has_value());
    EXPECT_LT(std::abs(estimate->samples + 300.0f), 0.5f)
        << "expected about -300, got " << estimate->samples;
}

// One sample at 48 kHz is 7 mm of path length, so sub-sample resolution is
// not a luxury for alignment work.
TEST(DelayFinder, AFractionalDelayIsResolvedBetweenSamples) {
    // Half-sample delay via linear interpolation of the source.
    const auto source = noise(kSize, 4);
    std::vector<float> measurement(source.size(), 0.0f);
    for (std::size_t i = 1; i < source.size(); ++i) {
        measurement[i] = 0.5f * source[i] + 0.5f * source[i - 1];
    }

    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto estimate = finder.find(source, measurement);
    ASSERT_TRUE(estimate.has_value());
    EXPECT_LT(std::abs(estimate->samples - 0.5f), 0.2f)
        << "expected about 0.5 samples, got " << estimate->samples;
    // And it must not be an integer, which would mean interpolation is off.
    EXPECT_GT(std::abs(estimate->samples - std::trunc(estimate->samples)), 0.01f);
}

TEST(DelayFinder, SecondsAndMetresFollowFromSamples) {
    const auto reference = noise(kSize, 5);
    // 480 samples at 48 kHz is 10 ms, about 3.43 m.
    const auto measurement = delayed(reference, 480);
    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto estimate = finder.find(reference, measurement);
    ASSERT_TRUE(estimate.has_value());

    EXPECT_LT(std::abs(estimate->seconds - 0.01f), 1e-4f) << estimate->seconds;
    EXPECT_LT(std::abs(estimate->metres() - 3.43f), 0.05f) << estimate->metres();
}

// The property PHAT exists for. With strong reflections, plain correlation
// smears and can latch onto the wrong arrival; PHAT should stay on the
// direct sound.
TEST(DelayFinder, PhatSurvivesReflectionsBetterThanPlainCorrelation) {
    constexpr std::size_t direct = 200;
    const auto source = noise(kSize, 6);

    // Direct arrival plus three strong, slightly later reflections - the
    // shape of a small room.
    std::vector<float> measurement(source.size(), 0.0f);
    const std::pair<std::size_t, float> arrivals[] = {
        {direct, 1.0f}, {263, 0.8f}, {341, 0.75f}, {455, 0.7f}};
    for (const auto& [delay, gain] : arrivals) {
        for (std::size_t i = delay; i < source.size(); ++i) {
            measurement[i] += source[i - delay] * gain;
        }
    }

    DelayFinder phat(kRate, kSize, Weighting::Phat);
    const auto with_phat = phat.find(source, measurement);
    ASSERT_TRUE(with_phat.has_value());
    EXPECT_LT(std::abs(with_phat->samples - static_cast<float>(direct)), 1.0f)
        << "PHAT should find the direct arrival at " << direct << ", got " << with_phat->samples;

    // PHAT's peak should also be far sharper relative to the background.
    DelayFinder plain(kRate, kSize, Weighting::None);
    const auto without = plain.find(source, measurement);
    ASSERT_TRUE(without.has_value());
    EXPECT_GT(with_phat->confidence, without->confidence)
        << "PHAT peak should be sharper: " << with_phat->confidence << " vs "
        << without->confidence;
}

TEST(DelayFinder, UncorrelatedSignalsGiveLowConfidence) {
    const auto reference = noise(kSize, 7);
    const auto measurement = noise(kSize, 8);

    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto unrelated = finder.find(reference, measurement);
    ASSERT_TRUE(unrelated.has_value());

    const auto matched = finder.find(reference, delayed(reference, 100));
    ASSERT_TRUE(matched.has_value());
    EXPECT_GT(matched->confidence, unrelated->confidence * 3.0f)
        << "a real delay should be far more confident: " << matched->confidence << " vs "
        << unrelated->confidence;
}

TEST(DelayFinder, SilenceYieldsNoEstimate) {
    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const std::vector<float> silence(kSize, 0.0f);
    const auto signal = noise(kSize, 9);
    const std::span<const float> empty;

    EXPECT_FALSE(finder.find(silence, silence).has_value());
    EXPECT_FALSE(finder.find(signal, silence).has_value());
    EXPECT_FALSE(finder.find(silence, signal).has_value());
    EXPECT_FALSE(finder.find(empty, empty).has_value());
}

TEST(DelayFinder, ShortInputIsZeroPaddedRatherThanRefused) {
    const auto reference = noise(1000, 10);
    const auto measurement = delayed(reference, 50);
    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto estimate = finder.find(reference, measurement);
    ASSERT_TRUE(estimate.has_value());
    EXPECT_LT(std::abs(estimate->samples - 50.0f), 1.0f) << estimate->samples;
}

TEST(DelayFinder, AFinderCanBeReusedWithoutCarryingState) {
    DelayFinder finder(kRate, kSize, Weighting::Phat);
    const auto a = noise(kSize, 11);
    const auto b = noise(kSize, 12);

    const auto first = finder.find(a, delayed(a, 111));
    const auto second = finder.find(b, delayed(b, 222));
    const auto third = finder.find(a, delayed(a, 111));
    ASSERT_TRUE(first.has_value() && second.has_value() && third.has_value());

    EXPECT_LT(std::abs(first->samples - 111.0f), 0.5f);
    EXPECT_LT(std::abs(second->samples - 222.0f), 0.5f);
    EXPECT_LT(std::abs(first->samples - third->samples), 1e-3f) << "repeat gave a different answer";
}

TEST(DelayFinder, TheAcousticDefaultCoversARealisticRoom) {
    const auto finder = DelayFinder::acoustic(kRate);
    // +-170 ms is about +-58 m of path, far beyond any real room.
    EXPECT_GE(finder.max_delay_samples(), 8000u);
}

TEST(DelayFinderDeathTest, AnOddSizeIsRejected) {
    EXPECT_DEATH(DelayFinder(kRate, 1023, Weighting::Phat), "correlation size must be even");
}

}  // namespace
}  // namespace analyzer::dsp
