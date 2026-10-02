// Ported from crates/analyzer-model/src/settings.rs.

#include "model/settings.hpp"

#include <cmath>
#include <string>

#include <gtest/gtest.h>

namespace analyzer::model {
namespace {

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

TEST(Settings, DefaultsRoundTripThroughText) {
    const Settings settings;
    EXPECT_EQ(Settings::from_text(settings.to_text()), settings);
}

TEST(Settings, APopulatedSettingsRoundTrips) {
    Settings settings;
    settings.fft_size = 32768;
    settings.window = WindowChoice::FlatTop;
    settings.averaging = AveragingChoice::Infinite;
    settings.start_on_launch = false;
    settings.min_hz = 10.0f;
    settings.max_hz = 24'000.0f;
    settings.min_db = -100.0f;
    settings.max_db = 12.0f;
    settings.level_grid_step = 10.0f;
    settings.spl_offset_db = 94.25f;
    settings.mic_cal_path = "/Users/someone/mic.frd";
    EXPECT_EQ(Settings::from_text(settings.to_text()), settings);
}

// The distinction the storage rules turn on: an unmeasured offset is not an
// offset of zero, and the file has to preserve that.
TEST(Settings, AnUnmeasuredSplOffsetIsNotZero) {
    const Settings unmeasured;
    EXPECT_EQ(unmeasured.spl_offset_db, std::nullopt);
    EXPECT_FALSE(contains(unmeasured.to_text(), "spl_offset_db"));

    Settings measured;
    measured.spl_offset_db = 0.0f;
    EXPECT_TRUE(contains(measured.to_text(), "spl_offset_db: 0"));
    EXPECT_EQ(Settings::from_text(measured.to_text()).spl_offset_db, std::optional(0.0f));
}

TEST(Settings, UnknownKeysAndCommentsAreIgnored) {
    const std::string text =
        "ANLZCFG1\n"
        "# written by a later version\n"
        "fft_size: 8192\n"
        "colour_scheme: midnight\n";
    const Settings settings = Settings::from_text(text);
    EXPECT_EQ(settings.fft_size, 8192u);
    EXPECT_EQ(settings.window, Settings{}.window);
}

TEST(Settings, AnUnparseableValueFallsBackToItsDefault) {
    const Settings settings = Settings::from_text("fft_size: enormous\nmin_db: -80");
    EXPECT_EQ(settings.fft_size, Settings{}.fft_size);
    EXPECT_EQ(settings.min_db, -80.0f);
}

// A truncated or garbage file must still open the app.
TEST(Settings, GarbageParsesToDefaults) {
    EXPECT_EQ(Settings::from_text(""), Settings{});
    EXPECT_EQ(Settings::from_text(std::string("\x00\x01nonsense", 10)), Settings{});
    EXPECT_EQ(Settings::from_text("ANLZCFG1\nfft_siz"), Settings{});
}

TEST(Settings, AnFftSizeOutsideTheOfferedSetIsRejected) {
    EXPECT_EQ(Settings::from_text("fft_size: 3000").fft_size, Settings{}.fft_size);
}

// Every guard here exists because the axis code divides by the span.
TEST(Settings, ValidationRepairsAnUnusableAxis) {
    const Settings repaired = Settings::from_text("min_hz: 0\nmax_hz: 0\nmin_db: 0\nmax_db: -50");
    EXPECT_GT(repaired.min_hz, 0.0f);
    EXPECT_GT(repaired.max_hz, repaired.min_hz);
    EXPECT_GT(repaired.max_db, repaired.min_db);
    EXPECT_GT(repaired.level_grid_step, 0.0f);
}

TEST(Settings, NonFiniteValuesAreRejected) {
    const Settings repaired = Settings::from_text("min_hz: NaN\nmax_db: inf\nspl_offset_db: NaN");
    EXPECT_TRUE(std::isfinite(repaired.min_hz));
    EXPECT_TRUE(std::isfinite(repaired.max_db));
    EXPECT_EQ(repaired.spl_offset_db, std::nullopt);
}

// A newline in a path would read back as a forged key, so it is dropped.
TEST(Settings, APathContainingANewlineIsNotWritten) {
    Settings settings;
    settings.mic_cal_path = "/tmp/a\nspl_offset_db: 200";
    const std::string text = settings.to_text();
    EXPECT_FALSE(contains(text, "spl_offset_db"));
    EXPECT_EQ(Settings::from_text(text).mic_cal_path, std::nullopt);
}

TEST(Settings, EveryWindowAndAveragingTokenRoundTrips) {
    for (const WindowChoice window : {WindowChoice::Rectangular, WindowChoice::Hann,
                                      WindowChoice::BlackmanHarris, WindowChoice::FlatTop}) {
        EXPECT_EQ(window_from_key(as_key(window)), std::optional(window));
    }
    for (const AveragingChoice averaging : {AveragingChoice::None, AveragingChoice::Fast,
                                            AveragingChoice::Infinite, AveragingChoice::PeakHold}) {
        EXPECT_EQ(averaging_from_key(as_key(averaging)), std::optional(averaging));
    }
}

}  // namespace
}  // namespace analyzer::model
