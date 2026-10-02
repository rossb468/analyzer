// Ported from crates/analyzer-ffi/src/lib.rs.

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"
#include "model/wav.hpp"
#include "test_util.hpp"

namespace analyzer::ffi {
namespace {

TEST(Generator, WritingASignalProducesRealAudio) {
    const std::filesystem::path path = test::scratch("signal.wav");
    std::filesystem::remove(path);
    const std::string c = test::c_path(path);

    AnalyzerStatus status = status_ok();
    const std::size_t frames =
        analyzer_write_signal(c.c_str(), AnalyzerSignal_Sine, 1000.0f, 0.0f, -6.0f, 0.5f, 48'000.0f,
                              AnalyzerSampleDepth_Float32, &status);
    EXPECT_EQ(status.code, 0) << test::message_of(status);
    EXPECT_EQ(frames, 24'000u);
    EXPECT_GT(std::filesystem::file_size(path), 90'000u);

    std::filesystem::remove(path);
}

// Silence is a valid stimulus setting and a meaningless file, so asking for one
// is refused rather than producing a file of zeroes.
TEST(Generator, WritingSilenceIsRefused) {
    const std::filesystem::path path = test::scratch("silence.wav");
    std::filesystem::remove(path);
    const std::string c = test::c_path(path);

    AnalyzerStatus status = status_ok();
    const std::size_t frames =
        analyzer_write_signal(c.c_str(), AnalyzerSignal_Silence, 1000.0f, 0.0f, -6.0f, 0.5f,
                              48'000.0f, AnalyzerSampleDepth_Float32, &status);
    EXPECT_EQ(frames, 0u);
    EXPECT_NE(status.code, 0);
    EXPECT_FALSE(std::filesystem::exists(path)) << "nothing should have been written";
}

// A level above full scale can only write something that clips.
TEST(Generator, TheWrittenLevelIsCappedAtFullScale) {
    const std::filesystem::path path = test::scratch("loud.wav");
    const std::string c = test::c_path(path);
    AnalyzerStatus status = status_ok();

    analyzer_write_signal(c.c_str(), AnalyzerSignal_Sine, 1000.0f, 0.0f,
                          // Asking for +20 dBFS.
                          20.0f, 0.1f, 48'000.0f, AnalyzerSampleDepth_Float32, &status);
    EXPECT_EQ(status.code, 0) << test::message_of(status);

    const model::WavFile wav = model::read_wav(path);
    float peak = 0.0f;
    for (const float sample : wav.samples) {
        peak = std::max(peak, std::fabs(sample));
    }
    EXPECT_LE(peak, 1.0f + 1e-6f) << "wrote " << peak << ", above full scale";

    std::filesystem::remove(path);
}

TEST(Generator, WritingASignalToleratesANullPath) {
    AnalyzerStatus status = status_ok();
    const std::size_t frames =
        analyzer_write_signal(nullptr, AnalyzerSignal_Sine, 1000.0f, 0.0f, -6.0f, 0.5f, 48'000.0f,
                              AnalyzerSampleDepth_Float32, &status);
    EXPECT_EQ(frames, 0u);
    EXPECT_NE(status.code, 0);
}

// A path that cannot be written reports the operating system's own reason.
TEST(Generator, AnUnwritablePathIsReportedWithItsReason) {
    const std::filesystem::path path = test::scratch("missing-dir") / "nested" / "out.wav";
    const std::string c = test::c_path(path);
    AnalyzerStatus status = status_ok();
    const std::size_t frames =
        analyzer_write_signal(c.c_str(), AnalyzerSignal_PinkNoise, 0.0f, 0.0f, -6.0f, 0.1f,
                              48'000.0f, AnalyzerSampleDepth_Int16, &status);
    EXPECT_EQ(frames, 0u);
    EXPECT_NE(status.code, 0);
    EXPECT_NE(test::message_of(status).find("writing "), std::string::npos);
}

}  // namespace
}  // namespace analyzer::ffi
