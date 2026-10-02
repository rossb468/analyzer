// Ported from crates/analyzer-ffi/src/lib.rs.

#include <vector>

#include <gtest/gtest.h>

#include "dsp/eq.hpp"
#include "engine/snapshot.hpp"
#include "ffi/eq_processor.hpp"

namespace analyzer::ffi {
namespace {

// The coefficients handed to the audio thread must describe the same filter the
// equaliser does, or what is heard and what is drawn diverge.
TEST(EqProcessor, PublishedCoefficientsMatchTheEqualiser) {
    dsp::Equaliser eq = dsp::Equaliser::graphic(48'000.0f);
    eq.set_gain_db(5, 6.0f);
    eq.set_preamp_db(-6.0f);

    const EqCoefficients coefficients = EqCoefficients::from_equaliser(1, eq);
    EXPECT_EQ(coefficients.count, 10u);
    EXPECT_NEAR(coefficients.trim, 0.501'187f, 1e-4f) << "-6 dB is 0.5012";

    const dsp::Biquad section = coefficients.sections[5];
    const dsp::Biquad expected = eq.bands()[5].design(48'000.0f);
    EXPECT_EQ(section.b0, expected.b0);
    EXPECT_EQ(section.a1, expected.a1);
}

// An equaliser with more bands than the array holds must not overflow it.
TEST(EqProcessor, PublishingStopsAtTheArrayBound) {
    std::vector<dsp::FilterBand> bands;
    for (std::size_t i = 0; i < kMaxEqBands + 8; ++i) {
        bands.push_back(
            dsp::FilterBand::peaking(100.0f + static_cast<float>(i) * 100.0f, 3.0f, 2.0f));
    }
    const dsp::Equaliser eq(48'000.0f, std::move(bands));
    const EqCoefficients coefficients = EqCoefficients::from_equaliser(1, eq);
    EXPECT_EQ(coefficients.count, kMaxEqBands);
}

// The processor must apply what was published, and must pick up a change.
TEST(EqProcessor, TheProcessorFollowsThePublishedCoefficients) {
    auto [publisher, reader] = engine::snapshot_channel(EqCoefficients{});
    EqProcessor processor(std::move(reader));

    // Nothing published yet: a pass-through.
    std::vector<float> samples = {1.0f, 0.0f, 0.0f, 0.0f};
    processor.process(samples);
    EXPECT_EQ(samples[0], 1.0f);

    // A pure trim is the easiest thing to check exactly.
    dsp::Equaliser eq = dsp::Equaliser::parametric(48'000.0f);
    eq.set_preamp_db(-6.0f);
    const EqCoefficients coefficients = EqCoefficients::from_equaliser(1, eq);
    publisher.publish_with([&](EqCoefficients& slot) { slot = coefficients; });

    samples = {1.0f, 0.0f, 0.0f, 0.0f};
    processor.process(samples);
    EXPECT_NEAR(samples[0], 0.501'187f, 1e-4f) << "the trim was not applied";
}

// A fader move must not click: the filter's memory survives a coefficient
// change, because only the coefficients are copied.
TEST(EqProcessor, ACoefficientChangeKeepsTheFilterState) {
    auto [publisher, reader] = engine::snapshot_channel(EqCoefficients{});
    EqProcessor processor(std::move(reader));

    dsp::Equaliser eq = dsp::Equaliser::parametric(48'000.0f);
    eq.push_band(dsp::FilterBand::peaking(1000.0f, 6.0f, 1.0f));
    publisher.publish_with(
        [&](EqCoefficients& slot) { slot = EqCoefficients::from_equaliser(1, eq); });

    // Excite the filter, then change a coefficient mid-signal.
    std::vector<float> block(64, 0.0f);
    block[0] = 1.0f;
    processor.process(block);

    eq.set_band(0, dsp::FilterBand::peaking(1000.0f, 5.0f, 1.0f));
    publisher.publish_with(
        [&](EqCoefficients& slot) { slot = EqCoefficients::from_equaliser(2, eq); });
    std::vector<float> next(8, 0.0f);
    processor.process(next);

    // The impulse response is still ringing into the next block. Had the delay
    // line been reset it would be exactly zero.
    EXPECT_NE(next[0], 0.0f);
}

}  // namespace
}  // namespace analyzer::ffi
