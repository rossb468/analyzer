// Ported from crates/analyzer-ffi/src/lib.rs.

#include <cmath>
#include <cstring>
#include <filesystem>
#include <string>

#include <gtest/gtest.h>

#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "test_util.hpp"

namespace analyzer::ffi {
namespace {

TEST(Settings, SurviveARoundTripThroughTheBoundary) {
    AnalyzerSettings settings = analyzer_settings_default();
    settings.fft_size = 16384;
    settings.window = AnalyzerWindow_FlatTop;
    settings.averaging = AnalyzerAveraging_Infinite;
    settings.has_spl_offset = true;
    settings.spl_offset_db = 94.5f;
    write_c_string(settings.mic_cal_path, "/tmp/mic.frd");

    const std::filesystem::path path = test::scratch("round-trip.cfg");
    const std::string c = test::c_path(path);
    AnalyzerStatus status = status_ok();
    EXPECT_TRUE(analyzer_settings_save(c.c_str(), &settings, &status));
    EXPECT_EQ(status.code, 0) << test::message_of(status);

    AnalyzerSettings loaded = analyzer_settings_default();
    EXPECT_TRUE(analyzer_settings_load(c.c_str(), &loaded, &status));
    EXPECT_EQ(loaded.fft_size, 16384u);
    EXPECT_EQ(loaded.window, AnalyzerWindow_FlatTop);
    EXPECT_EQ(loaded.averaging, AnalyzerAveraging_Infinite);
    EXPECT_TRUE(loaded.has_spl_offset);
    EXPECT_NEAR(loaded.spl_offset_db, 94.5f, 1e-6f);
    EXPECT_EQ(read_c_string(loaded.mic_cal_path), "/tmp/mic.frd");

    std::filesystem::remove(path);
}

// First launch. A missing file is the normal case, not an error, and reporting
// it as one would train the UI to ignore the status.
TEST(Settings, AMissingSettingsFileLoadsDefaultsAndSucceeds) {
    const std::filesystem::path path = test::scratch("absent.cfg");
    std::filesystem::remove(path);
    const std::string c = test::c_path(path);

    AnalyzerStatus status = status_failure("clobber me");
    model::Settings other;
    other.fft_size = 1024;
    AnalyzerSettings loaded = to_c(other);
    EXPECT_TRUE(analyzer_settings_load(c.c_str(), &loaded, &status));
    EXPECT_EQ(status.code, 0);
    EXPECT_EQ(loaded.fft_size, model::Settings{}.fft_size);
}

// The flag exists so that "never calibrated" cannot be confused with a
// calibration that came out at zero.
TEST(Settings, AnUnmeasuredSplOffsetStaysUnmeasuredAcrossTheBoundary) {
    const AnalyzerSettings defaults = analyzer_settings_default();
    EXPECT_FALSE(defaults.has_spl_offset);
    EXPECT_EQ(to_settings(defaults).spl_offset_db, std::nullopt);

    AnalyzerSettings measured = defaults;
    measured.has_spl_offset = true;
    measured.spl_offset_db = 0.0f;
    EXPECT_EQ(to_settings(measured).spl_offset_db, std::optional<float>(0.0f));
}

// A hand-edited file must not be able to hand the axis code a zero span.
TEST(Settings, ACorruptSettingsFileLoadsAsSomethingUsable) {
    const std::filesystem::path path = test::scratch("corrupt.cfg");
    test::write_text(path, "min_hz: 0\nmax_hz: 0\nfft_size: 7\n");
    const std::string c = test::c_path(path);

    AnalyzerStatus status = status_ok();
    AnalyzerSettings loaded = analyzer_settings_default();
    EXPECT_TRUE(analyzer_settings_load(c.c_str(), &loaded, &status));
    EXPECT_GT(loaded.min_hz, 0.0f);
    EXPECT_GT(loaded.max_hz, loaded.min_hz);
    EXPECT_EQ(loaded.fft_size, model::Settings{}.fft_size);

    std::filesystem::remove(path);
}

TEST(Settings, CallsTolerateNullPointers) {
    AnalyzerSettings settings = analyzer_settings_default();
    AnalyzerStatus status = status_ok();
    EXPECT_FALSE(analyzer_settings_load(nullptr, &settings, &status));
    EXPECT_NE(status.code, 0);
    EXPECT_FALSE(analyzer_settings_save(nullptr, &settings, &status));
    EXPECT_NE(status.code, 0);

    const std::string c = test::c_path(test::scratch("null.cfg"));
    EXPECT_FALSE(analyzer_settings_load(c.c_str(), nullptr, &status));
    EXPECT_FALSE(analyzer_settings_save(c.c_str(), nullptr, &status));
    // A null status pointer is legal and must not be written through.
    EXPECT_FALSE(analyzer_settings_save(nullptr, nullptr, nullptr));
}

// Truncation must land on a character boundary or the buffer stops being valid
// UTF-8 and the path reads back as replacement characters.
TEST(Settings, AnOverlongPathTruncatesWithoutSplittingACharacter) {
    char buffer[8] = {};
    write_c_string(buffer, "\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9\xC3\xA9");  // seven é
    const std::string read = read_c_string(buffer);
    EXPECT_TRUE(test::only_repeats_of(read, "\xC3\xA9")) << "got " << read;
    EXPECT_LE(read.size(), 7u);
}

// A settings file that is a directory is a real failure, and says so, with the
// defaults still handed back so the UI has something to show.
TEST(Settings, AnUnreadableLocationIsReportedNotSilentlyDefaulted) {
    const std::filesystem::path dir = test::scratch("a-directory");
    std::filesystem::create_directories(dir);
    const std::string c = test::c_path(dir);

    AnalyzerStatus status = status_ok();
    AnalyzerSettings loaded = analyzer_settings_default();
    loaded.fft_size = 1024;
    EXPECT_FALSE(analyzer_settings_load(c.c_str(), &loaded, &status));
    EXPECT_NE(status.code, 0);
    EXPECT_NE(test::message_of(status).find("reading "), std::string::npos);
    EXPECT_EQ(loaded.fft_size, model::Settings{}.fft_size);

    std::filesystem::remove(dir);
}

// Saving creates the directory the file belongs in.
TEST(Settings, SavingCreatesTheContainingDirectory) {
    const std::filesystem::path root = test::scratch("tree");
    std::filesystem::remove_all(root);
    const std::filesystem::path path = root / "deep" / "settings.cfg";
    const std::string c = test::c_path(path);

    const AnalyzerSettings settings = analyzer_settings_default();
    AnalyzerStatus status = status_failure("clobber me");
    EXPECT_TRUE(analyzer_settings_save(c.c_str(), &settings, &status));
    EXPECT_EQ(status.code, 0);
    EXPECT_TRUE(std::filesystem::exists(path));

    std::filesystem::remove_all(root);
}

}  // namespace
}  // namespace analyzer::ffi
