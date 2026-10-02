// Ported from crates/analyzer-dsp/src/fft.rs, plus the inverse transform,
// which the Rust core reached through realfft directly.

#include "dsp/fft.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "support/signals.hpp"

namespace analyzer::dsp {
namespace {

using analyzer::test::kTau;

constexpr std::size_t kSize = 4096;

TEST(RealFft, BinsIsHalfSizePlusOne) {
    const RealFft fft(1024);
    EXPECT_EQ(fft.size(), 1024u);
    EXPECT_EQ(fft.bins(), 513u);
}

TEST(RealFft, DcInputLandsEntirelyInBinZero) {
    RealFft fft(kSize);
    std::vector<Complex32> out(fft.bins());
    const std::vector<float> input(kSize, 1.0f);

    fft.forward(input, out);

    // Unnormalised transform: full-scale DC gives `size` in bin 0.
    EXPECT_NEAR(out[0].real(), static_cast<float>(kSize), 1e-2f);
    EXPECT_NEAR(out[0].imag(), 0.0f, 1e-3f);
    for (std::size_t k = 1; k < out.size(); ++k) {
        ASSERT_LT(std::abs(out[k]), 1e-2f) << "bin " << k << " should be empty";
    }
}

TEST(RealFft, BinCentredSineRecoversItsAmplitude) {
    RealFft fft(kSize);
    std::vector<Complex32> out(fft.bins());
    constexpr std::size_t bin = 64;
    constexpr float amplitude = 0.5f;

    // Exactly `bin` whole cycles across the frame, so all energy lands in one
    // bin and there is nothing to leak.
    const auto input = test::sine(kSize, static_cast<float>(bin), kSize, amplitude);

    fft.forward(input, out);

    const auto peak = std::max_element(
        out.begin(), out.end(), [](Complex32 a, Complex32 b) { return std::abs(a) < std::abs(b); });
    EXPECT_EQ(static_cast<std::size_t>(peak - out.begin()), bin);

    // One-sided amplitude for 0 < k < size/2 is 2|X[k]| / size.
    const float recovered = 2.0f * std::abs(out[bin]) / static_cast<float>(kSize);
    EXPECT_NEAR(recovered, amplitude, 1e-3f);
}

TEST(RealFft, ParsevalEnergyIsConserved) {
    RealFft fft(kSize);
    std::vector<Complex32> out(fft.bins());
    // Two incommensurate tones plus DC, so the check is not accidentally
    // passing on a single clean bin.
    std::vector<float> input(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        const float t = static_cast<float>(n) / static_cast<float>(kSize);
        input[n] = 0.1f + 0.3f * std::sin(kTau * 37.0f * t) + 0.2f * std::cos(kTau * 211.0f * t);
    }

    fft.forward(input, out);

    float time_energy = 0.0f;
    for (const float x : input) {
        time_energy += x * x;
    }

    // For real input the one-sided spectrum double-counts everything except
    // DC and Nyquist.
    const std::size_t last = out.size() - 1;
    float spectral = std::norm(out[0]) + std::norm(out[last]);
    float middle = 0.0f;
    for (std::size_t k = 1; k < last; ++k) {
        middle += std::norm(out[k]);
    }
    spectral = (spectral + 2.0f * middle) / static_cast<float>(kSize);

    EXPECT_LT(std::abs(time_energy - spectral) / time_energy, 1e-4f);
}

TEST(RealFft, InputIsNotModified) {
    RealFft fft(kSize);
    std::vector<Complex32> out(fft.bins());
    std::vector<float> input(kSize);
    for (std::size_t n = 0; n < kSize; ++n) {
        input[n] = std::sin(static_cast<float>(n) * 0.01f);
    }
    const auto before = input;

    fft.forward(input, out);

    EXPECT_EQ(input, before) << "forward must not consume the caller's input";
}

// forward then inverse scales by size(), the convention deconvolution and the
// delay finder rely on.
TEST(RealFft, InverseUndoesForwardScaledBySize) {
    RealFft fft(1024);
    std::vector<float> input(1024);
    for (std::size_t n = 0; n < input.size(); ++n) {
        input[n] = std::sin(static_cast<float>(n) * 0.37f) +
                   0.25f * std::cos(static_cast<float>(n) * 1.9f);
    }
    std::vector<Complex32> spectrum(fft.bins());
    std::vector<float> round_trip(fft.size());

    fft.forward(input, spectrum);
    fft.inverse(spectrum, round_trip);

    for (std::size_t n = 0; n < input.size(); ++n) {
        ASSERT_NEAR(round_trip[n] / 1024.0f, input[n], 1e-4f) << "at " << n;
    }
}

// Sizes that are not powers of two still work, as they did with realfft.
TEST(RealFft, NonPowerOfTwoSizesWork) {
    for (const std::size_t size :
         {std::size_t{2}, std::size_t{6}, std::size_t{30}, std::size_t{1000}}) {
        RealFft fft(size);
        const std::vector<float> input(size, 1.0f);
        std::vector<Complex32> out(fft.bins());
        fft.forward(input, out);
        EXPECT_NEAR(out[0].real(), static_cast<float>(size), 1e-3f) << "size " << size;
    }
}

TEST(RealFftDeathTest, WrongInputLengthAborts) {
    RealFft fft(kSize);
    std::vector<Complex32> out(fft.bins());
    const std::vector<float> input(kSize - 1, 0.0f);
    EXPECT_DEATH(fft.forward(input, out), "input length must equal");
}

TEST(RealFftDeathTest, OddSizeIsRejected) {
    EXPECT_DEATH(RealFft(1023), "FFT size must be even");
}

}  // namespace
}  // namespace analyzer::dsp
