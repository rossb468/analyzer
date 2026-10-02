// Ported from crates/analyzer-ffi/src/lib.rs.

#include <cmath>
#include <cstddef>
#include <string>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"

namespace analyzer::ffi {
namespace {

// Every entry point must survive a null handle. A UI that has not started a
// session yet will call these, and crashing is not an acceptable answer.
TEST(Session, NullHandlesAreToleratedEverywhere) {
    analyzer_device_list_destroy(nullptr);
    EXPECT_EQ(analyzer_device_list_count(nullptr), 0u);
    EXPECT_FALSE(analyzer_device_list_get(nullptr, 0, nullptr));

    analyzer_session_stop(nullptr);
    EXPECT_EQ(analyzer_session_copy_transfer(nullptr, AnalyzerCurve_Magnitude, nullptr, 0), 0u);
    EXPECT_FALSE(analyzer_session_transfer_info(nullptr, nullptr));
    EXPECT_FALSE(analyzer_session_estimate_delay(nullptr));
    EXPECT_FALSE(analyzer_session_set_delay(nullptr, 0));
    EXPECT_FALSE(analyzer_session_set_signal(nullptr, AnalyzerSignal_PinkNoise, -20.0f, 1000.0f));
    EXPECT_FALSE(analyzer_session_set_eq_mode(nullptr, AnalyzerEqMode_Graphic));
    EXPECT_EQ(analyzer_session_eq_band_count(nullptr), 0u);
    EXPECT_FALSE(analyzer_session_eq_get_band(nullptr, 0, nullptr));
    EXPECT_FALSE(analyzer_session_eq_set_band(nullptr, 0, nullptr));
    EXPECT_FALSE(analyzer_session_eq_set_gain(nullptr, 0, 0.0f));
    EXPECT_EQ(analyzer_session_eq_add_band(nullptr, nullptr), -1);
    EXPECT_FALSE(analyzer_session_eq_remove_band(nullptr, 0));
    EXPECT_FALSE(analyzer_session_eq_flatten(nullptr));
    EXPECT_FALSE(analyzer_session_eq_trim(nullptr));
    EXPECT_FALSE(analyzer_session_eq_set_preamp(nullptr, 0.0f));
    EXPECT_FALSE(analyzer_session_eq_info(nullptr, nullptr));
    EXPECT_EQ(analyzer_session_copy_eq_curve(nullptr, -1, nullptr, 0), 0u);
    EXPECT_EQ(analyzer_session_copy_corrected(nullptr, nullptr, 0), 0u);
    EXPECT_TRUE(std::isnan(analyzer_phase_to_y(nullptr, 0.0f)));
    EXPECT_TRUE(std::isnan(analyzer_coherence_to_y(nullptr, 0.0f)));
    EXPECT_FALSE(analyzer_session_has_new_frame(nullptr));
    EXPECT_EQ(analyzer_session_copy_trace(nullptr, nullptr, 0), 0u);
    EXPECT_EQ(analyzer_session_copy_average(nullptr, nullptr, 0), 0u);
    EXPECT_FALSE(analyzer_session_reset_average(nullptr));
    EXPECT_FALSE(analyzer_session_distortion(nullptr, 0.0f, nullptr));
    EXPECT_FALSE(analyzer_session_save_measurement(nullptr, nullptr, nullptr, 0.0f, nullptr));
    EXPECT_FALSE(analyzer_session_export_text(nullptr, nullptr, nullptr, 0.0f, nullptr));
    EXPECT_FALSE(analyzer_session_frame_info(nullptr, nullptr));
    EXPECT_EQ(analyzer_session_device_name(nullptr, nullptr, 0), 0u);
    EXPECT_FALSE(analyzer_session_set_plot(nullptr, 100.0f, 100.0f, 20.0f, 20'000.0f, -120.0f, 0.0f,
                                           AnalyzerReduction_Max));
    EXPECT_TRUE(std::isnan(analyzer_freq_to_x(nullptr, 1000.0f)));
    EXPECT_TRUE(std::isnan(analyzer_x_to_freq(nullptr, 100.0f)));
    EXPECT_TRUE(std::isnan(analyzer_db_to_y(nullptr, -20.0f)));
    EXPECT_TRUE(std::isnan(analyzer_y_to_db(nullptr, 100.0f)));
    EXPECT_EQ(analyzer_frequency_ticks(nullptr, nullptr, 0), 0u);
    EXPECT_EQ(analyzer_level_ticks(nullptr, 10.0f, nullptr, 0), 0u);
}

TEST(Session, StartingWithANullConfigReportsRatherThanCrashes) {
    AnalyzerStatus status = status_ok();
    AnalyzerSession* session = analyzer_session_start(nullptr, &status);
    EXPECT_EQ(session, nullptr);
    EXPECT_NE(status.code, 0);
}

TEST(Session, DefaultConfigIsUsable) {
    const AnalyzerSessionConfig config = analyzer_session_config_default();
    EXPECT_EQ(config.device_uid, nullptr) << "null means default device";
    EXPECT_EQ(config.fft_size, 4096u);
    EXPECT_EQ(config.fft_size % 2, 0u);
    EXPECT_EQ(config.mode, AnalyzerMode_Spectrum) << "a UI opens on the RTA";
    EXPECT_EQ(config.signal, AnalyzerSignal_Silence) << "nothing plays until asked";
    EXPECT_LE(config.signal_level_db, -12.0f)
        << "the default stimulus level must be quiet enough not to damage anything";
}

// Where no backend exists the start fails honestly, with a message, rather than
// returning a handle that does nothing.
TEST(Session, StartingWithoutADeviceReportsWhy) {
    AnalyzerSessionConfig config = analyzer_session_config_default();
    AnalyzerStatus status = status_ok();
    AnalyzerSession* session = analyzer_session_start(&config, &status);
    if (session != nullptr) {
        // A platform with a real default input: nothing to assert but a clean stop.
        EXPECT_EQ(status.code, 0);
        analyzer_session_stop(session);
        return;
    }
    EXPECT_NE(status.code, 0);
    EXPECT_NE(status.message[0], 0) << "a failure must say why";
}

// An odd transform size is refused before any device is touched.
TEST(Session, AnOddFftSizeIsRefusedUpFront) {
    AnalyzerSessionConfig config = analyzer_session_config_default();
    config.fft_size = 1001;
    AnalyzerStatus status = status_ok();
    EXPECT_EQ(analyzer_session_start(&config, &status), nullptr);
    EXPECT_NE(status.code, 0);
    EXPECT_NE(std::string(status.message).find("fft size must be even"), std::string::npos);
}

// A device identifier that is not text cannot name a device.
TEST(Session, ADeviceUidThatIsNotUtf8IsRefused) {
    AnalyzerSessionConfig config = analyzer_session_config_default();
    config.device_uid = "\xFF\xFE";
    AnalyzerStatus status = status_ok();
    EXPECT_EQ(analyzer_session_start(&config, &status), nullptr);
    EXPECT_NE(std::string(status.message).find("not valid UTF-8"), std::string::npos);
}

}  // namespace
}  // namespace analyzer::ffi
