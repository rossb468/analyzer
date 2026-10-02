// Ported from crates/analyzer-dsp/src/ir.rs.

#include "dsp/ir.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "dsp/deconv.hpp"
#include "dsp/window.hpp"
#include "support/generated_signals.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48000.0f;

// A synthetic decay with a known reverberation time.
//
// Amplitude falls as `e^(-t/tau)`, so energy falls at `8.686/tau` dB per
// second and a 60 dB fall takes `6.908 * tau`.
ImpulseResponse decaying_noise(float rt60, float seconds, std::uint64_t seed) {
    const float tau = rt60 / 6.908f;
    const auto count = static_cast<std::size_t>(kRate * seconds);

    auto samples = test::white_noise(count, 1.0f, seed);
    for (std::size_t index = 0; index < samples.size(); ++index) {
        const float t = static_cast<float>(index) / kRate;
        samples[index] *= std::exp(-t / tau);
    }

    return {.samples = std::move(samples), .peak_samples = 0.0f, .sample_rate = kRate};
}

ImpulseResponse spike_at(std::size_t delay, std::size_t length) {
    std::vector<float> samples(length, 0.0f);
    samples[delay] = 1.0f;
    return {.samples = std::move(samples),
            .peak_samples = static_cast<float>(delay),
            .sample_rate = kRate};
}

float sum_of_magnitudes(std::span<const float> samples) {
    return std::accumulate(samples.begin(), samples.end(), 0.0f,
                           [](float sum, float s) { return sum + std::abs(s); });
}

// The headline: a synthetic decay with a known RT60 must measure as that.
TEST(ReverbTime, AKnownDecayMeasuresItsReverberationTime) {
    for (const float target : {0.3f, 0.6f, 1.2f}) {
        const auto ir = decaying_noise(target, target * 3.0f, 1);
        const auto decay = schroeder_decay(ir);
        const auto rt = reverb_time(decay, kRate);

        const float t20 = rt.t20.value_or(0.0f);
        const float t30 = rt.t30.value_or(0.0f);
        EXPECT_LT(std::abs(t20 - target) / target, 0.08f)
            << "T20 read " << t20 << " for a " << target << " s decay";
        EXPECT_LT(std::abs(t30 - target) / target, 0.08f)
            << "T30 read " << t30 << " for a " << target << " s decay";
    }
}

// For a pure exponential the three estimates describe the same line, so
// they must agree. Disagreement is the diagnostic for a decay that is not
// straight, and it should not fire on one that is.
TEST(ReverbTime, TheEstimatesAgreeForAStraightDecay) {
    const auto ir = decaying_noise(0.8f, 3.0f, 2);
    const auto rt = reverb_time(schroeder_decay(ir), kRate);

    ASSERT_TRUE(rt.edt.has_value() && rt.t20.has_value() && rt.t30.has_value());
    const auto spread = rt.spread();
    ASSERT_TRUE(spread.has_value());
    EXPECT_LT(*spread, 0.1f) << "estimates spread by " << *spread << ": edt " << *rt.edt << ", t20 "
                             << *rt.t20 << ", t30 " << *rt.t30;
    EXPECT_LT(std::abs(*rt.best() - 0.8f), 0.08f);
}

TEST(SchroederDecay, TheDecayCurveStartsAtZeroAndFallsMonotonically) {
    const auto ir = decaying_noise(0.5f, 2.0f, 3);
    const auto decay = schroeder_decay(ir);

    ASSERT_FALSE(decay.empty());
    EXPECT_LT(std::abs(decay[0]), 1e-4f) << "curve should start at 0 dB";
    for (std::size_t i = 1; i < decay.size(); ++i) {
        ASSERT_LE(decay[i], decay[i - 1] + 1e-4f)
            << "backward integration cannot increase: " << decay[i - 1] << " -> " << decay[i];
    }
}

TEST(ReverbTime, SilenceProducesNoReverberationEstimate) {
    const ImpulseResponse ir{
        .samples = std::vector<float>(4096, 0.0f), .peak_samples = 0.0f, .sample_rate = kRate};
    const auto rt = reverb_time(schroeder_decay(ir), kRate);
    EXPECT_FALSE(rt.best().has_value());
    EXPECT_FALSE(rt.spread().has_value());
}

// A decay that never falls far enough cannot yield T30, and must say so
// rather than reporting the truncation artefact as a reverberation time.
//
// This is subtler than it looks. The backward integral always terminates at
// negative infinity, so a truncated curve *does* pass -35 dB - just at the
// very end, and for reasons that have nothing to do with the room.
TEST(ReverbTime, ATruncatedDecayDoesNotReportTheCliffAsReverberation) {
    // A 2 s reverberation time recorded for only 0.35 s: about 10 dB of real
    // decay, then the integral falls off its cliff.
    const auto ir = decaying_noise(2.0f, 0.35f, 4);
    const auto decay = schroeder_decay(ir);

    // The curve genuinely does reach -35 dB, which is the trap.
    EXPECT_TRUE(std::any_of(decay.begin(), decay.end(), [](float db) { return db <= -35.0f; }))
        << "the terminal plunge should be present in the raw curve";

    const auto rt = reverb_time(decay, kRate);
    EXPECT_FALSE(rt.t30.has_value())
        << "T30 must not be taken from the truncation cliff, got " << rt.t30.value_or(0.0f);
}

TEST(ApplyGate, GatingKeepsOnlyTheRequestedSpan) {
    const auto ir = spike_at(1000, 8192);
    const Gate gate{.start_seconds = -0.001f, .end_seconds = 0.005f, .fade_seconds = 0.001f};
    const auto gated = apply_gate(ir, gate);

    const auto expected = static_cast<std::ptrdiff_t>((0.005f + 0.001f) * kRate);
    EXPECT_LT(std::abs(static_cast<std::ptrdiff_t>(gated.size()) - expected), 4)
        << "kept " << gated.size() << " samples, expected about " << expected;
}

// The point of gating: a late reflection outside the gate must not appear
// in the response.
TEST(ApplyGate, GatingExcludesALateReflection) {
    auto ir = spike_at(1000, 8192);
    // A strong reflection 10 ms later.
    ir.samples[1000 + static_cast<std::size_t>(0.010f * kRate)] = 0.8f;

    const auto tight = Gate::anechoic(0.005f);
    const auto wide = Gate::anechoic(0.020f);

    const float inside = sum_of_magnitudes(apply_gate(ir, tight));
    const float outside = sum_of_magnitudes(apply_gate(ir, wide));
    EXPECT_GT(outside, inside + 0.5f)
        << "the wider gate should include the reflection: " << inside << " vs " << outside;
}

// The cost of gating, stated rather than hidden. A 5 ms gate cannot resolve
// below 200 Hz, which is why quasi-anechoic measurements are spliced in the
// bass.
TEST(Gate, AGateReportsTheResolutionItCosts) {
    const Gate gate{.start_seconds = 0.0f, .end_seconds = 0.005f, .fade_seconds = 0.001f};
    EXPECT_LT(std::abs(gate.length_seconds() - 0.005f), 1e-6f);
    EXPECT_LT(std::abs(gate.resolution_hz() - 200.0f), 0.1f);

    const Gate long_gate{.start_seconds = 0.0f, .end_seconds = 0.5f, .fade_seconds = 0.01f};
    EXPECT_LT(std::abs(long_gate.resolution_hz() - 2.0f), 0.01f);
}

// A single spike is a flat system, so its gated response must be flat.
TEST(GatedResponse, ASpikeHasAFlatGatedResponse) {
    const auto ir = spike_at(500, 8192);
    const auto response = gated_response(ir, Gate::anechoic(0.010f), 4096);
    ASSERT_TRUE(response.has_value());

    // Check well above the gate's resolution limit.
    const auto first =
        static_cast<std::size_t>(response->resolution_hz * 2.0f / response->bin_spacing_hz);
    const auto levels = std::span<const float>(response->magnitude_db)
                            .subspan(first, response->magnitude_db.size() / 2 - first);
    const float mean =
        std::accumulate(levels.begin(), levels.end(), 0.0f) / static_cast<float>(levels.size());
    for (std::size_t index = 0; index < levels.size(); ++index) {
        ASSERT_LT(std::abs(levels[index] - mean), 0.5f)
            << "bin " << index << " deviates: " << levels[index] << " vs mean " << mean;
    }
}

TEST(GatedResponse, TheResponseReportsWhichFrequenciesToBelieve) {
    const auto ir = spike_at(500, 8192);
    const auto response = gated_response(ir, Gate::anechoic(0.005f), 4096);
    ASSERT_TRUE(response.has_value());

    EXPECT_FALSE(response->is_trustworthy(50.0f)) << "below a 5 ms gate's limit";
    EXPECT_TRUE(response->is_trustworthy(1000.0f));
    EXPECT_LT(std::abs(response->bin_spacing_hz - kRate / 4096.0f), 1e-3f);
}

TEST(GatedResponse, DegenerateGatesAndSizesAreRefused) {
    const auto ir = spike_at(500, 8192);

    // Closes before it opens.
    const Gate backwards{.start_seconds = 0.010f, .end_seconds = 0.001f, .fade_seconds = 0.0f};
    EXPECT_TRUE(apply_gate(ir, backwards).empty());
    EXPECT_FALSE(gated_response(ir, backwards, 4096).has_value());

    // Odd transform size.
    EXPECT_FALSE(gated_response(ir, Gate::anechoic(0.005f), 4095).has_value());
}

TEST(ApplyGate, AnEmptyImpulseResponseGatesToNothing) {
    const ImpulseResponse ir{.samples = {}, .peak_samples = 0.0f, .sample_rate = kRate};
    EXPECT_TRUE(apply_gate(ir, Gate::anechoic(0.005f)).empty());
    EXPECT_TRUE(schroeder_decay(ir).empty());
}

// The closing taper is what stops a hard truncation smearing the response.
TEST(ApplyGate, TheGateTapersItsClosingEdge) {
    const ImpulseResponse ir{
        .samples = std::vector<float>(8192, 1.0f), .peak_samples = 0.0f, .sample_rate = kRate};
    const Gate gate{.start_seconds = 0.0f, .end_seconds = 0.010f, .fade_seconds = 0.004f};
    const auto gated = apply_gate(ir, gate);

    ASSERT_FALSE(gated.empty());
    EXPECT_LT(std::abs(gated.front() - 1.0f), 1e-6f) << "the open edge is untouched";
    EXPECT_LT(std::abs(gated.back()), 0.01f)
        << "the closing edge should reach zero, got " << gated.back();
    // And it should be a smooth ramp, not a step.
    const float middle = gated[gated.size() - gated.size() / 8];
    EXPECT_TRUE(middle > 0.0f && middle < 1.0f) << "mid-fade was " << middle;
}

TEST(GateWindow, AGateWindowMatchesTheGateLength) {
    const Gate gate{.start_seconds = 0.0f, .end_seconds = 0.010f, .fade_seconds = 0.002f};
    const auto window = gate_window(gate, kRate);
    ASSERT_TRUE(window.has_value());
    EXPECT_EQ(window->size(), static_cast<std::size_t>(0.010f * kRate));
    EXPECT_EQ(recommended_gate_window(), WindowKind::rectangular());
}

}  // namespace
}  // namespace analyzer::dsp
