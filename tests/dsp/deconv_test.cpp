// Ported from crates/analyzer-dsp/src/deconv.rs.

#include "dsp/deconv.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "support/generated_signals.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48000.0f;
constexpr std::size_t kLength = 16384;

using test::sweep;
using test::white_noise;

// A recording of `source` arriving `delay` samples late.
//
// The buffer is *longer* than the stimulus, because a real recording keeps
// running after the stimulus stops. Truncating it to the stimulus length
// would throw away exactly the part that arrived late.
std::vector<float> delayed(std::span<const float> source, std::size_t delay, float gain) {
    std::vector<float> out(source.size() + delay, 0.0f);
    for (std::size_t i = 0; i < source.size(); ++i) {
        out[i + delay] = source[i] * gain;
    }
    return out;
}

// A deconvolver large enough for a stimulus of `kLength` plus a late tail.
Deconvolver make_deconvolver() {
    return Deconvolver(kRate, kLength + 8192);
}

float energy(std::span<const float> samples) {
    return std::accumulate(samples.begin(), samples.end(), 0.0f,
                           [](float sum, float s) { return sum + s * s; });
}

// The defining behaviour: a system that only delays must deconvolve to a
// single spike at that delay.
TEST(Deconvolver, APureDelayDeconvolvesToASpike) {
    const auto stimulus = sweep(kLength);
    for (const std::size_t delay :
         {std::size_t{0}, std::size_t{1}, std::size_t{64}, std::size_t{500}, std::size_t{3000}}) {
        const auto response = delayed(stimulus, delay, 1.0f);
        auto deconvolver = make_deconvolver();
        const auto ir = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
        ASSERT_TRUE(ir.has_value());

        EXPECT_LT(std::abs(ir->peak_samples - static_cast<float>(delay)), 1.0f)
            << "delay " << delay << " peaked at " << ir->peak_samples;
    }
}

// And the spike must be a spike: energy concentrated, not smeared.
TEST(Deconvolver, TheRecoveredImpulseIsConcentrated) {
    const auto stimulus = sweep(kLength);
    const auto response = delayed(stimulus, 100, 1.0f);
    auto deconvolver = make_deconvolver();
    const auto ir = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
    ASSERT_TRUE(ir.has_value());

    const float peak = ir->peak_amplitude();
    const auto centre = static_cast<std::size_t>(std::round(ir->peak_samples));

    // Almost all the energy should sit within a few samples of the arrival.
    const float total = energy(ir->samples);
    const std::size_t from = centre > 8 ? centre - 8 : 0;
    const std::size_t to = std::min(centre + 8, ir->samples.size());
    const float near = energy(std::span<const float>(ir->samples).subspan(from, to - from));
    EXPECT_GT(near / total, 0.9f) << "only " << 100.0f * near / total
                                  << "% of energy is near the peak";
    EXPECT_GT(peak, 0.0f);
}

TEST(Deconvolver, AGainScalesTheImpulse) {
    const auto stimulus = sweep(kLength);
    auto deconvolver = make_deconvolver();

    const auto unity =
        deconvolver.deconvolve(stimulus, delayed(stimulus, 50, 1.0f), kDefaultRegularisation);
    ASSERT_TRUE(unity.has_value());
    const float unity_peak = unity->peak_amplitude();
    const auto halved =
        deconvolver.deconvolve(stimulus, delayed(stimulus, 50, 0.5f), kDefaultRegularisation);
    ASSERT_TRUE(halved.has_value());
    const float halved_peak = halved->peak_amplitude();

    const float ratio = 20.0f * std::log10(halved_peak / unity_peak);
    EXPECT_LT(std::abs(ratio + 6.0206f), 0.3f) << "expected -6 dB, got " << ratio;
}

// Several arrivals must all appear, in the right places and at the right
// relative levels - this is a room impulse response in miniature.
TEST(Deconvolver, MultipleReflectionsAllAppear) {
    const auto stimulus = sweep(kLength);
    const std::pair<std::size_t, float> arrivals[] = {{200, 1.0f}, {450, 0.5f}, {900, 0.25f}};

    std::vector<float> response(stimulus.size() + 1024, 0.0f);
    for (const auto& [delay, gain] : arrivals) {
        for (std::size_t i = 0; i < stimulus.size(); ++i) {
            response[i + delay] += stimulus[i] * gain;
        }
    }

    auto deconvolver = make_deconvolver();
    const auto ir = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
    ASSERT_TRUE(ir.has_value());

    const float peak = ir->peak_amplitude();
    for (const auto& [delay, gain] : arrivals) {
        float local = 0.0f;
        for (std::size_t i = delay - 3; i < delay + 4; ++i) {
            local = std::fmax(local, std::abs(ir->samples[i]));
        }
        const float relative = local / peak;
        EXPECT_LT(std::abs(relative - gain), 0.08f)
            << "arrival at " << delay << " read " << relative << ", expected " << gain;
    }
}

// Zero padding to twice the length is what stops the reverberant tail
// wrapping around onto the direct sound and looking like a pre-echo.
TEST(Deconvolver, TheTransformIsPaddedAgainstCircularWrap) {
    const Deconvolver deconvolver(kRate, 10000);
    EXPECT_GE(deconvolver.size(), 20000u);
    EXPECT_TRUE(std::has_single_bit(deconvolver.size()));
    EXPECT_GE(deconvolver.max_length(), 10000u);
}

TEST(Deconvolver, ALateArrivalDoesNotWrapOntoTheStart) {
    const auto stimulus = sweep(4096);
    // An arrival at 3900 is almost a whole stimulus length late; with a
    // circular transform it would fold back onto the start.
    const auto response = delayed(stimulus, 3900, 1.0f);
    Deconvolver deconvolver(kRate, response.size());
    const auto ir = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
    ASSERT_TRUE(ir.has_value());

    EXPECT_LT(std::abs(ir->peak_samples - 3900.0f), 2.0f)
        << "peaked at " << ir->peak_samples << " instead of 3900";
}

// Regularisation is the difference between a usable measurement and noise
// amplified into nonsense where the stimulus had no energy.
TEST(Deconvolver, RegularisationSuppressesNoiseOutsideTheSweepBand) {
    // A sweep that stops at 1 kHz leaves everything above it unexcited.
    const auto stimulus = sweep(kLength, 100.0f, 1000.0f);

    // Response is the stimulus plus broadband noise the stimulus cannot
    // explain above 1 kHz.
    const auto noise = white_noise(kLength, 0.01f, 3);
    auto response = delayed(stimulus, 100, 1.0f);
    // The delayed copy is 100 samples longer than the noise; the sum is as long
    // as the shorter of the two.
    response.resize(noise.size());
    for (std::size_t i = 0; i < response.size(); ++i) {
        response[i] += noise[i];
    }

    auto deconvolver = make_deconvolver();
    const auto regularised = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
    ASSERT_TRUE(regularised.has_value());
    const auto barely = deconvolver.deconvolve(stimulus, response, 1e-12f);
    ASSERT_TRUE(barely.has_value());

    // The meaningful question is how much of the recovered energy actually
    // lands on the impulse. The system here is a pure 100-sample delay, so
    // the ideal answer is a single spike and anything elsewhere is the
    // unexcited bands amplifying noise.
    //
    // Two earlier attempts at this assertion measured the wrong thing.
    // Peak-to-mean stopped discriminating once the impulse was trimmed to
    // its causal region, and a peak-relative noise floor is blind precisely
    // when it matters: with almost no regularisation the amplified noise
    // *becomes* the peak, so dividing by it hides the failure.
    const auto concentration = [](const ImpulseResponse& ir) -> float {
        const float total = energy(ir.samples);
        if (total <= 0.0f) {
            return 0.0f;
        }
        const float near = ir.samples.size() >= 111
                               ? energy(std::span<const float>(ir.samples).subspan(90, 21))
                               : 0.0f;
        return near / total;
    };

    EXPECT_GT(concentration(*regularised), concentration(*barely) * 2.0f)
        << "regularisation should concentrate energy on the true impulse: "
        << concentration(*regularised) * 100.0f << "% vs " << concentration(*barely) * 100.0f
        << "%";
    // Deliberately not asserting the peak lands at 100. With a sweep
    // covering only a third of the spectrum and broadband noise across all
    // of it, regularisation substantially improves the result without
    // rescuing it - the global peak can still sit in the unexcited region.
    // Claiming otherwise would be asserting something this scenario does not
    // support.
}

TEST(Deconvolver, SilenceYieldsNothing) {
    auto deconvolver = make_deconvolver();
    const std::vector<float> silence(1000, 0.0f);
    const auto stimulus = sweep(1000);
    const std::span<const float> empty;

    EXPECT_FALSE(deconvolver.deconvolve(silence, silence, kDefaultRegularisation).has_value());
    EXPECT_FALSE(deconvolver.deconvolve(empty, stimulus, kDefaultRegularisation).has_value());
    EXPECT_FALSE(deconvolver.deconvolve(stimulus, empty, kDefaultRegularisation).has_value());
}

TEST(Deconvolver, OversizedInputIsRefusedRatherThanTruncated) {
    Deconvolver deconvolver(kRate, 1024);
    const std::vector<float> long_signal(4096, 0.5f);
    EXPECT_FALSE(
        deconvolver.deconvolve(long_signal, long_signal, kDefaultRegularisation).has_value())
        << "silently truncating would produce a plausible but wrong response";
}

TEST(ImpulseResponse, TimingHelpersAreRelativeToTheArrival) {
    const auto stimulus = sweep(kLength);
    const auto response = delayed(stimulus, 480, 1.0f);
    auto deconvolver = make_deconvolver();
    const auto ir = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
    ASSERT_TRUE(ir.has_value());

    // The arrival itself is t = 0, and 480 samples earlier is -10 ms.
    EXPECT_LT(std::abs(ir->time_at(480)), 1e-4f) << ir->time_at(480);
    EXPECT_LT(std::abs(ir->time_at(0) + 0.01f), 1e-4f) << ir->time_at(0);
    EXPECT_GT(ir->duration_seconds(), 0.0f);
}

// Deconvolution should not care what the stimulus was, which is the whole
// reason for doing it this way rather than with a matched filter.
TEST(Deconvolver, NoiseWorksAsAStimulusToo) {
    const auto stimulus = white_noise(kLength, 0.5f, 4);

    const auto response = delayed(stimulus, 321, 1.0f);
    auto deconvolver = make_deconvolver();
    const auto ir = deconvolver.deconvolve(stimulus, response, kDefaultRegularisation);
    ASSERT_TRUE(ir.has_value());

    EXPECT_LT(std::abs(ir->peak_samples - 321.0f), 1.0f) << "peaked at " << ir->peak_samples;
}

TEST(DeconvolverDeathTest, AZeroLengthDeconvolverIsRejected) {
    EXPECT_DEATH(Deconvolver(kRate, 0), "max_length must be non-zero");
}

}  // namespace
}  // namespace analyzer::dsp
