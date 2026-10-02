// Ported from crates/analyzer-dsp/src/generator.rs.

#include "dsp/generator.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "dsp/complex.hpp"
#include "dsp/fft.hpp"
#include "dsp/window.hpp"

namespace analyzer::dsp {
namespace {

constexpr float kRate = 48'000.0f;
constexpr std::size_t kSize = 4096;
// Bin 85 exactly at 48 kHz / 4096.
constexpr float kOnBinHz = 996.09375f;

// Stand-in for the Rust SpectrumAnalyzer, which is ported separately: a Welch
// average of 50%-overlapped, windowed frames, in the analyzer's own power
// convention (mean-square per bin, a bin-centred sine of amplitude A reads
// A^2 / 2). Averaging is the incremental mean the analyzer uses for
// Averaging::Infinite.
std::vector<float> average_power(std::span<const float> samples, std::size_t size,
                                 WindowKind kind) {
    const Window window(kind, size);
    RealFft fft(size);
    const float scale = 1.0f / (static_cast<float>(size) * window.coherent_gain());
    const std::size_t hop = size / 2;

    std::vector<float> windowed(size);
    std::vector<Complex32> spectrum(fft.bins());
    std::vector<float> power(fft.bins(), 0.0f);
    const std::size_t last = fft.bins() - 1;

    std::size_t frames = 0;
    for (std::size_t start = 0; start + size <= samples.size(); start += hop) {
        window.apply_to(samples.subspan(start, size), windowed);
        fft.forward(windowed, spectrum);
        ++frames;
        const auto n = static_cast<float>(frames);
        for (std::size_t k = 0; k < power.size(); ++k) {
            // DC and Nyquist are real and unpaired; every bin between them
            // stands for a conjugate pair.
            const float magnitude = std::abs(spectrum[k]) * scale;
            const float frame_power =
                (k == 0 || k == last) ? magnitude * magnitude : 2.0f * magnitude * magnitude;
            power[k] += (frame_power - power[k]) / n;
        }
    }
    return power;
}

// 0 dBFS is a full-scale sine, which has mean square 0.5.
std::vector<float> analyse(std::span<const float> samples, std::size_t size, WindowKind window) {
    std::vector<float> db = average_power(samples, size, window);
    for (float& value : db) {
        value = value > 0.0f ? std::max(10.0f * std::log10(2.0f * value), -200.0f) : -200.0f;
    }
    return db;
}

std::vector<float> analyse(std::span<const float> samples, WindowKind window) {
    return analyse(samples, kSize, window);
}

std::vector<float> generate(Signal signal, std::size_t samples) {
    Generator generator(kRate, signal, 12345);
    std::vector<float> out(samples, 0.0f);
    generator.fill(out);
    return out;
}

std::size_t peak_bin(std::span<const float> db) {
    return static_cast<std::size_t>(std::max_element(db.begin(), db.end()) - db.begin());
}

float peak_abs(std::span<const float> samples) {
    float peak = 0.0f;
    for (const float s : samples) {
        peak = std::max(peak, std::abs(s));
    }
    return peak;
}

float mean_around(std::span<const float> db, float hz) {
    const float spacing = kRate / static_cast<float>(kSize);
    const auto centre = static_cast<std::size_t>(hz / spacing);
    const std::size_t first = centre > 40 ? centre - 40 : 0;
    const std::size_t last = std::min(centre + 40, db.size());
    float sum = 0.0f;
    for (std::size_t i = first; i < last; ++i) {
        sum += db[i];
    }
    return sum / static_cast<float>(last - first);
}

// Closes the loop: the generator's own sine, measured by our own analyzer,
// must land in the right bin at the right level.
TEST(Generator, AGeneratedSineMeasuresAtItsOwnFrequencyAndLevel) {
    const auto samples = generate(Signal::sine(kOnBinHz, 0.5f), kSize * 8);
    const auto db = analyse(samples, WindowKind::flat_top());

    EXPECT_EQ(peak_bin(db), 85u);
    EXPECT_LT(std::abs(db[85] - -6.0206f), 0.05f) << "expected -6.02 dBFS, got " << db[85];
}

// The reason the accumulator is double. Generate ten seconds and check the tone
// has not wandered off its bin.
TEST(Generator, ALongSineDoesNotDriftOffFrequency) {
    Generator generator(kRate, Signal::sine(kOnBinHz, 0.5f), 1);
    std::vector<float> discard(static_cast<std::size_t>(kRate) * 10);
    generator.fill(discard);

    // Now measure a fresh window ten seconds in.
    std::vector<float> tail(kSize * 4);
    generator.fill(tail);
    const auto db = analyse(tail, WindowKind::flat_top());
    EXPECT_EQ(peak_bin(db), 85u) << "tone drifted after ten seconds";
}

TEST(Generator, SilenceIsSilent) {
    const auto samples = generate(Signal::silence(), 1024);
    EXPECT_TRUE(std::all_of(samples.begin(), samples.end(), [](float s) { return s == 0.0f; }));
}

TEST(Generator, AmplitudeIsRespectedAndClamped) {
    for (const float amplitude : {0.1f, 0.5f, 1.0f}) {
        const auto samples = generate(Signal::sine(1000.0f, amplitude), 48'000);
        const float peak = peak_abs(samples);
        EXPECT_LT(std::abs(peak - amplitude), 0.01f)
            << "amplitude " << amplitude << " peaked at " << peak;
    }
    // Out-of-range requests clamp rather than clipping the converter.
    const auto hot = generate(Signal::sine(1000.0f, 5.0f), 4096);
    EXPECT_LE(peak_abs(hot), 1.001f);
}

TEST(Generator, NoSignalExceedsFullScale) {
    for (const Signal signal : {
             Signal::white_noise(1.0f),
             Signal::pink_noise(1.0f),
             Signal::sine(997.0f, 1.0f),
             Signal::sweep(20.0f, 20'000.0f, 1.0f, 1.0f, true),
         }) {
        const auto samples = generate(signal, 96'000);
        const float peak = peak_abs(samples);
        EXPECT_LE(peak, 1.001f) << to_string(signal.kind) << " peaked at " << peak;
    }
}

// White noise is equal energy per hertz, so a linear-frequency spectrum is
// flat. Compared across two decades it should not tilt.
TEST(Generator, WhiteNoiseIsSpectrallyFlat) {
    const auto samples = generate(Signal::white_noise(0.5f), kSize * 64);
    const auto db = analyse(samples, WindowKind::hann());

    const float low = mean_around(db, 200.0f);
    const float high = mean_around(db, 10'000.0f);
    EXPECT_LT(std::abs(low - high), 1.5f)
        << "white noise tilted: " << low << " dB at 200 Hz vs " << high << " at 10 kHz";
}

// Pink noise is equal energy per octave, which on a per-bin spectrum means
// falling at 3 dB per octave. Across the ~5.6 octaves from 200 Hz to 10 kHz
// that is about 17 dB.
TEST(Generator, PinkNoiseFallsThreeDecibelsPerOctave) {
    const auto samples = generate(Signal::pink_noise(0.5f), kSize * 64);
    const auto db = analyse(samples, WindowKind::hann());

    const float low = mean_around(db, 200.0f);
    const float high = mean_around(db, 10'000.0f);
    const float octaves = std::log2(10'000.0f / 200.0f);
    const float slope = (low - high) / octaves;
    EXPECT_LT(std::abs(slope - 3.0f), 0.6f) << "expected about -3 dB/octave, measured " << slope
                                            << " (" << low << " -> " << high << ")";
}

// Reproducible noise matters: a flaky spectrum assertion is worse than no
// assertion at all.
TEST(Generator, NoiseIsReproducibleForAGivenSeed) {
    const auto seeded = [](std::uint64_t seed) {
        Generator generator(kRate, Signal::white_noise(0.5f), seed);
        std::vector<float> out(4096, 0.0f);
        generator.fill(out);
        return out;
    };
    EXPECT_EQ(seeded(42), seeded(42)) << "same seed must give the same noise";
    EXPECT_NE(seeded(42), seeded(43)) << "different seeds must differ";
}

// The Rust version of this test went through generate(), which seeds with
// 12345, so it never reached the zero-seed path it is named for. Here the seed
// really is zero.
TEST(Generator, AZeroSeedStillProducesNoise) {
    Generator generator(kRate, Signal::white_noise(0.5f), 0);
    std::vector<float> samples(1024, 0.0f);
    generator.fill(samples);
    EXPECT_TRUE(std::any_of(samples.begin(), samples.end(), [](float s) { return s != 0.0f; }))
        << "zero is a fixed point of xorshift and must be replaced";
}

// The sweep must actually sweep: its first moments should be low frequency and
// its last high.
TEST(Generator, ASweepStartsLowAndEndsHigh) {
    const float seconds = 2.0f;
    Generator generator(kRate, Signal::sweep(50.0f, 10'000.0f, seconds, 0.5f, false), 1);
    const auto total = static_cast<std::size_t>(kRate * seconds);
    std::vector<float> all(total, 0.0f);
    generator.fill(all);

    const auto peak_hz = [](std::span<const float> chunk) {
        constexpr std::size_t size = 2048;
        const auto power = average_power(chunk, size, WindowKind::hann());
        return static_cast<float>(peak_bin(power)) * (kRate / static_cast<float>(size));
    };

    const float start = peak_hz(std::span<const float>(all).first(8192));
    const float end = peak_hz(std::span<const float>(all).last(8192));
    EXPECT_LT(start, 200.0f) << "sweep started at " << start << " Hz, expected near 50";
    EXPECT_GT(end, 5000.0f) << "sweep ended at " << end << " Hz, expected near 10k";
}

TEST(Generator, ANonRepeatingSweepFinishesAndGoesQuiet) {
    Generator generator(kRate, Signal::sweep(100.0f, 1000.0f, 0.1f, 0.5f, false), 1);
    std::vector<float> out(static_cast<std::size_t>(kRate * 0.2f), 0.0f);
    generator.fill(out);

    EXPECT_TRUE(generator.is_finished());
    const auto tail = std::span<const float>(out).last(1000);
    EXPECT_TRUE(std::all_of(tail.begin(), tail.end(), [](float s) { return s == 0.0f; }))
        << "should be silent after the sweep";
}

TEST(Generator, ARepeatingSweepNeverFinishes) {
    Generator generator(kRate, Signal::sweep(100.0f, 1000.0f, 0.05f, 0.5f, true), 1);
    std::vector<float> out(static_cast<std::size_t>(kRate * 0.5f), 0.0f);
    generator.fill(out);
    EXPECT_FALSE(generator.is_finished());
    EXPECT_TRUE(std::any_of(out.begin(), out.end(), [](float s) { return std::abs(s) > 0.1f; }))
        << "should still be sounding";
}

// Buffer size is the driver's business, not the generator's.
TEST(Generator, ChunkedGenerationMatchesOneBlock) {
    const Signal signal = Signal::sine(997.0f, 0.5f);
    Generator whole(kRate, signal, 7);
    std::vector<float> one(5000, 0.0f);
    whole.fill(one);

    Generator chunked(kRate, signal, 7);
    std::vector<float> many(5000, 0.0f);
    for (std::size_t offset = 0; offset < many.size(); offset += 113) {
        const std::size_t count = std::min<std::size_t>(113, many.size() - offset);
        chunked.fill(std::span<float>(many).subspan(offset, count));
    }

    for (std::size_t i = 0; i < one.size(); ++i) {
        ASSERT_LT(std::abs(one[i] - many[i]), 1e-6f)
            << "diverged at sample " << i << ": " << one[i] << " vs " << many[i];
    }
}

TEST(Generator, ResetReturnsToTheBeginning) {
    Generator generator(kRate, Signal::sine(1000.0f, 0.5f), 1);
    std::vector<float> first(512, 0.0f);
    generator.fill(first);

    generator.reset();
    std::vector<float> again(512, 0.0f);
    generator.fill(again);
    EXPECT_EQ(first, again);
}

TEST(Generator, ChangingSignalResetsState) {
    Generator generator(kRate, Signal::sine(1000.0f, 0.5f), 1);
    std::vector<float> discard(1024, 0.0f);
    generator.fill(discard);

    generator.set_signal(Signal::silence());
    EXPECT_EQ(generator.signal(), Signal::silence());
    generator.fill(discard);
    EXPECT_TRUE(std::all_of(discard.begin(), discard.end(), [](float s) { return s == 0.0f; }));
}

TEST(GeneratorDeathTest, ANonPositiveSampleRateIsRejected) {
    EXPECT_DEATH(Generator(0.0f, Signal::silence(), 1), "sample rate must be positive");
}

}  // namespace
}  // namespace analyzer::dsp
