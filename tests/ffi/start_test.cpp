// New in the C++ port: the rust tests could not start a session without a
// device, so nothing exercised the audio callback or the session's wiring.
//
// start_session() takes the backend as a parameter, which lets these tests drive
// a whole session from the offline backend: the callback runs on the test's own
// thread, under the allocation trap this binary links, while the analysis
// thread runs for real.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "audio/offline.hpp"
#include "dsp/generator.hpp"
#include "engine/rt.hpp"
#include "ffi/session.hpp"
#include "test_util.hpp"

namespace analyzer::ffi {
namespace {

using namespace std::chrono_literals;

constexpr double kRate = 48'000.0;

// Deterministic noise, so a run is the same every time.
audio::Source noise(std::size_t frames, std::size_t channels) {
    std::vector<float> samples(frames * channels);
    std::uint32_t state = 12345;
    for (float& sample : samples) {
        state = state * 1'664'525u + 1'013'904'223u;
        sample = (static_cast<float>(state >> 8) / static_cast<float>(1u << 24) - 0.5f) * 0.2f;
    }
    return audio::Source(std::move(samples), channels, kRate);
}

AnalyzerSessionConfig quiet_config() {
    AnalyzerSessionConfig config = analyzer_session_config_default();
    config.fft_size = 1024;
    config.buffer_frames = 256;
    config.overlap = AnalyzerOverlap_Half;
    config.averaging = AnalyzerAveraging_None;
    return config;
}

std::string failure_of(const AnalyzerSessionConfig& config, audio::AudioBackend& backend,
                       const std::optional<std::string>& uid = std::nullopt) {
    try {
        start_session(config, uid, backend);
    } catch (const std::exception& error) {
        return error.what();
    }
    ADD_FAILURE() << "the session started, but should have been refused";
    return {};
}

audio::OfflineStream& offline(AnalyzerSession& session) {
    return dynamic_cast<audio::OfflineStream&>(*session.stream);
}

// Poll until `condition` holds. The analysis thread runs in real time, so the
// tests wait for what they need rather than sleeping a guess.
bool wait_for(const std::function<bool()>& condition, std::chrono::milliseconds limit = 10s) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (!condition()) {
        if (std::chrono::steady_clock::now() > deadline) {
            return false;
        }
        std::this_thread::sleep_for(2ms);
    }
    return true;
}

float peak_of(std::span<const float> samples) {
    float peak = 0.0f;
    for (const float sample : samples) {
        peak = std::max(peak, std::fabs(sample));
    }
    return peak;
}

// This binary links analyzer::alloc_trap. If it ever stops doing so, every test
// below that runs the callback passes vacuously - so check that first.
TEST(StartSession, TheAllocationTrapIsInstalledInTheTestBinary) {
    EXPECT_TRUE(engine::alloc_trap_installed());
}

// ---------------------------------------------------------------- refusals --

TEST(StartSession, AnOddOrTinyFftSizeIsRefused) {
    audio::OfflineBackend backend(noise(512, 2), 128);
    AnalyzerSessionConfig config = quiet_config();
    config.fft_size = 1001;
    EXPECT_EQ(failure_of(config, backend), "fft size must be even and at least 2, got 1001");
    config.fft_size = 0;
    EXPECT_EQ(failure_of(config, backend), "fft size must be even and at least 2, got 0");
}

TEST(StartSession, AnUnknownDeviceIsNamedInTheMessage) {
    audio::OfflineBackend backend(noise(512, 2), 128);
    EXPECT_EQ(failure_of(quiet_config(), backend, std::string("nope")),
              "no device with uid 'nope'");
}

TEST(StartSession, AChannelBeyondTheDeviceIsRefused) {
    audio::OfflineBackend backend(noise(512, 2), 128);
    AnalyzerSessionConfig config = quiet_config();
    config.channel = 5;
    EXPECT_EQ(failure_of(config, backend), "channel 5 requested but Offline (in-memory) has 2");
}

TEST(StartSession, AStimulusNeedsAnOutputChannel) {
    audio::OfflineBackend backend(noise(512, 2), 128);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_PinkNoise;
    config.output_mask = 0;
    EXPECT_EQ(failure_of(config, backend),
              "a stimulus was requested but no output channel was selected; Offline (in-memory) "
              "has 2 output(s)");
}

TEST(StartSession, TheReferenceAndMeasurementChannelsMustDiffer) {
    audio::OfflineBackend backend(noise(512, 2), 128);
    AnalyzerSessionConfig config = quiet_config();
    config.mode = AnalyzerMode_Transfer;
    config.reference = AnalyzerReference_Input;
    config.channel = 0;
    config.reference_channel = 0;
    EXPECT_NE(failure_of(config, backend).find("must differ"), std::string::npos);

    config.reference_channel = 7;
    EXPECT_EQ(failure_of(config, backend),
              "reference channel 7 requested but Offline (in-memory) has 2");
}

TEST(StartSession, AnInternalReferenceNeedsAStimulus) {
    audio::OfflineBackend backend(noise(512, 2), 128);
    AnalyzerSessionConfig config = quiet_config();
    config.mode = AnalyzerMode_Transfer;
    config.reference = AnalyzerReference_Internal;
    config.signal = AnalyzerSignal_Silence;
    EXPECT_NE(failure_of(config, backend).find("an internal reference needs a stimulus"),
              std::string::npos);
}

// ------------------------------------------------------------------ analysis --

TEST(StartSession, ASilentSessionPublishesFramesThatReachTheCaller) {
    audio::OfflineBackend backend(noise(8192, 2), 256);
    std::unique_ptr<AnalyzerSession> session = start_session(quiet_config(), std::nullopt, backend);
    AnalyzerSession* handle = session.get();

    EXPECT_EQ(offline(*session).config().output_channels.size(), 0u) << "nothing plays until asked";
    EXPECT_EQ(offline(*session).run_to_end(), 8192u);
    ASSERT_TRUE(wait_for([&] { return handle->engine.published_count() > 0; }));

    ASSERT_TRUE(analyzer_session_set_plot(handle, 400.0f, 300.0f, 20.0f, 20'000.0f, -120.0f, 0.0f,
                                          AnalyzerReduction_Max));
    float trace[512] = {};
    EXPECT_EQ(analyzer_session_copy_trace(handle, trace, 512), 400u);
    EXPECT_EQ(analyzer_session_copy_average(handle, trace, 512), 400u);

    AnalyzerFrameInfo info{};
    ASSERT_TRUE(analyzer_session_frame_info(handle, &info));
    EXPECT_GT(info.sequence, 0u);
    EXPECT_EQ(info.sample_rate, 48'000.0f);
    EXPECT_EQ(info.overruns, 0u) << "dropped audio invalidates a measurement";

    char name[64] = {};
    const std::uintptr_t length = analyzer_session_device_name(handle, name, sizeof(name));
    EXPECT_EQ(std::string(name, length), "Offline (in-memory)");
    // A buffer too small for the name is cut, and still terminated.
    char small[5] = {};
    EXPECT_EQ(analyzer_session_device_name(handle, small, sizeof(small)), 4u);
    EXPECT_EQ(std::string(small), "Offl");

    analyzer_session_stop(session.release());
}

// A transfer function against the generator's own samples: the stimulus is
// spliced into the ring as one more channel, in the same block as the input.
TEST(StartSession, AnInternalReferenceFeedsTheTransferFunction) {
    audio::OfflineBackend backend(noise(16'384, 2), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.mode = AnalyzerMode_Transfer;
    config.reference = AnalyzerReference_Internal;
    config.signal = AnalyzerSignal_PinkNoise;
    config.signal_level_db = -20.0f;
    config.output_mask = 0b01;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);
    AnalyzerSession* handle = session.get();

    // The graphic equaliser is in the path, so the reference carries what was
    // actually played.
    ASSERT_TRUE(analyzer_session_set_eq_mode(handle, AnalyzerEqMode_Graphic));
    ASSERT_TRUE(analyzer_session_eq_set_gain(handle, 5, 6.0f));

    offline(*session).run_to_end();
    ASSERT_TRUE(wait_for([&] {
        AnalyzerTransferInfo info{};
        return analyzer_session_transfer_info(handle, &info) && info.frames > 0;
    }));

    ASSERT_TRUE(analyzer_session_set_plot(handle, 200.0f, 100.0f, 20.0f, 20'000.0f, -120.0f, 0.0f,
                                          AnalyzerReduction_Max));
    float curve[256] = {};
    for (const AnalyzerCurve kind :
         {AnalyzerCurve_Magnitude, AnalyzerCurve_Phase, AnalyzerCurve_Coherence}) {
        EXPECT_EQ(analyzer_session_copy_transfer(handle, kind, curve, 256), 200u);
    }
    EXPECT_TRUE(analyzer_session_estimate_delay(handle));
    EXPECT_TRUE(analyzer_session_set_delay(handle, 32));
    analyzer_session_stop(session.release());
}

// -------------------------------------------------------------------- output --

TEST(StartSession, ThePlayedStimulusReachesEverySelectedOutputChannel) {
    audio::OfflineBackend backend(noise(2048, 2), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_Sine;
    config.signal_hz = 1000.0f;
    config.signal_level_db = -6.0f;
    config.output_mask = 0b11;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);

    offline(*session).run_to_end();
    const std::span<const float> played = offline(*session).captured_output();
    ASSERT_EQ(played.size(), 2048u * 2);
    EXPECT_NEAR(peak_of(played), 0.5012f, 2e-3f) << "-6 dBFS";
    // One mono stimulus, copied: a transfer function measures one path.
    for (std::size_t frame = 0; frame < 2048; ++frame) {
        ASSERT_EQ(played[frame * 2], played[frame * 2 + 1]) << "frame " << frame;
    }
    analyzer_session_stop(session.release());
}

TEST(StartSession, TheOutputMaskChoosesTheChannels) {
    audio::OfflineBackend backend(noise(1024, 2), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_WhiteNoise;
    config.output_mask = 0b10;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);

    ASSERT_EQ(offline(*session).config().output_channels, std::vector<std::uint32_t>{1});
    offline(*session).run_to_end();
    EXPECT_EQ(offline(*session).captured_output().size(), 1024u) << "one channel interleaved";
    analyzer_session_stop(session.release());
}

// The equaliser sits in front of the output, so a trim must change what is
// played, not only what is drawn.
TEST(StartSession, TheEqualiserShapesTheStimulus) {
    audio::OfflineBackend backend(noise(2048, 2), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_Sine;
    config.signal_level_db = 0.0f;
    config.output_mask = 0b01;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);

    ASSERT_TRUE(analyzer_session_set_eq_mode(session.get(), AnalyzerEqMode_Parametric));
    ASSERT_TRUE(analyzer_session_eq_set_preamp(session.get(), -6.0f));
    offline(*session).run_to_end();
    EXPECT_NEAR(peak_of(offline(*session).captured_output()), 0.5012f, 2e-3f);
    analyzer_session_stop(session.release());
}

TEST(StartSession, ChangingTheSignalTakesEffectOnTheNextBlock) {
    audio::OfflineBackend backend(noise(1024, 2), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_Sine;
    config.signal_level_db = -6.0f;
    config.output_mask = 0b01;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);

    ASSERT_EQ(offline(*session).pump(), 256u);
    EXPECT_GT(peak_of(offline(*session).captured_output()), 0.4f);

    ASSERT_TRUE(analyzer_session_set_signal(session.get(), AnalyzerSignal_Silence, -20.0f, 0.0f));
    ASSERT_EQ(offline(*session).pump(), 256u);
    const std::span<const float> played = offline(*session).captured_output();
    EXPECT_EQ(peak_of(played.subspan(256)), 0.0f);

    // Bad numbers are refused rather than reaching the generator.
    EXPECT_FALSE(analyzer_session_set_signal(session.get(), AnalyzerSignal_Sine,
                                             std::numeric_limits<float>::quiet_NaN(), 1000.0f));
    analyzer_session_stop(session.release());
}

// Opening an output stream is a device operation. A session started silent has
// none, and asking for a signal later cannot conjure one.
TEST(StartSession, ASessionStartedSilentStaysSilent) {
    audio::OfflineBackend backend(noise(512, 2), 256);
    std::unique_ptr<AnalyzerSession> session = start_session(quiet_config(), std::nullopt, backend);

    ASSERT_TRUE(analyzer_session_set_signal(session.get(), AnalyzerSignal_Sine, -6.0f, 1000.0f));
    offline(*session).run_to_end();
    EXPECT_TRUE(offline(*session).captured_output().empty());
    analyzer_session_stop(session.release());
}

// A device may hand over more than it promised. The excess is dropped, counted
// as an overrun, and never allocated for: this runs under the allocation trap.
TEST(StartSession, AnOversizedCallbackIsClampedNotGrown) {
    constexpr std::size_t kFrames = kMaxCallbackFrames + 3000;
    audio::OfflineBackend backend(noise(kFrames, 2), kFrames);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_Sine;
    config.signal_level_db = -6.0f;
    config.output_mask = 0b01;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);

    ASSERT_EQ(offline(*session).pump(), kFrames);
    const std::span<const float> played = offline(*session).captured_output();
    ASSERT_EQ(played.size(), kFrames);
    EXPECT_GT(peak_of(played.first(kMaxCallbackFrames)), 0.4f);
    EXPECT_EQ(peak_of(played.subspan(kMaxCallbackFrames)), 0.0f)
        << "the tail must be silence, not stale audio";
    analyzer_session_stop(session.release());
}

// -------------------------------------------------------------- measurement --

// A perfect wire: the recorded response is the sweep itself, so deconvolving it
// must give a single impulse, at the origin, which the whole chain reports.
TEST(StartSession, ASweptMeasurementOfAPerfectWireFindsAnImpulseAtTheOrigin) {
    AnalyzerMeasureConfig measure = analyzer_measure_config_default();
    measure.start_hz = 100.0f;
    measure.end_hz = 10'000.0f;
    measure.seconds = 0.5f;
    measure.tail_seconds = 0.5f;
    measure.level_db = 0.0f;

    constexpr std::size_t kFrames = 48'000;
    std::vector<float> wire(kFrames, 0.0f);
    dsp::Generator generator(48'000.0f,
                             dsp::Signal::sweep(100.0f, 10'000.0f, 0.5f, 1.0f, /*repeat=*/false),
                             kGeneratorSeed);
    generator.fill(wire);

    audio::OfflineBackend backend(audio::Source::mono(std::move(wire), kRate), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_Sine;
    config.signal_level_db = -120.0f;
    config.output_mask = 0b01;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);
    AnalyzerSession* handle = session.get();

    AnalyzerStatus status = status_ok();
    AnalyzerMeasureResult result{};
    EXPECT_FALSE(analyzer_session_finish_measurement(handle, &result, &status));
    EXPECT_EQ(test::message_of(status), "no measurement is running");

    ASSERT_TRUE(analyzer_session_start_measurement(handle, &measure, &status))
        << test::message_of(status);
    AnalyzerMeasureProgress progress{};
    ASSERT_TRUE(analyzer_session_measure_progress(handle, &progress));
    EXPECT_TRUE(progress.active);
    EXPECT_EQ(progress.total, kFrames);
    EXPECT_FALSE(progress.complete);

    // Still playing, so there is nothing to finish yet.
    EXPECT_FALSE(analyzer_session_finish_measurement(handle, &result, &status));
    EXPECT_EQ(test::message_of(status), "the sweep is still playing");

    // Feed the ring at a few times real time, leaving the analysis thread room
    // to drain it: a dropped block would make the recording wrong.
    while (offline(*session).pump() > 0) {
        std::this_thread::sleep_for(1ms);
    }
    ASSERT_TRUE(wait_for(
        [&] { return analyzer_session_measure_progress(handle, &progress) && progress.complete; }));

    ASSERT_TRUE(analyzer_session_finish_measurement(handle, &result, &status))
        << test::message_of(status);
    EXPECT_EQ(status.code, 0);
    EXPECT_LT(std::fabs(result.arrival_ms), 1.0f) << "the peak is the reference";
    EXPECT_GT(result.peak_amplitude, 0.0f);
    EXPECT_GT(result.points, 0u);

    EXPECT_TRUE(analyzer_session_has_measurement(handle));
    AnalyzerMeasureResult again{};
    ASSERT_TRUE(analyzer_session_measurement_result(handle, &again));
    EXPECT_EQ(again.points, result.points);

    ASSERT_TRUE(analyzer_session_set_plot(handle, 300.0f, 200.0f, 20.0f, 20'000.0f, -120.0f, 0.0f,
                                          AnalyzerReduction_Max));
    float response[300] = {};
    EXPECT_EQ(analyzer_session_copy_measured(handle, response, 300), 300u);

    // Normalised to the peak, so the arrival reads as one whatever the level.
    float impulse[64] = {};
    ASSERT_EQ(analyzer_session_copy_impulse(handle, impulse, 64, 0.05f), 64u);
    EXPECT_NEAR(peak_of(impulse), 1.0f, 1e-3f);

    // A finished measurement leaves the generator silent.
    EXPECT_FALSE(analyzer_session_measure_progress(handle, &progress) && progress.active);
    analyzer_session_stop(session.release());
}

TEST(StartSession, ACancelledMeasurementSilencesTheGeneratorAndForgetsTheRun) {
    audio::OfflineBackend backend(noise(2048, 2), 256);
    AnalyzerSessionConfig config = quiet_config();
    config.signal = AnalyzerSignal_Sine;
    config.output_mask = 0b01;
    std::unique_ptr<AnalyzerSession> session = start_session(config, std::nullopt, backend);
    AnalyzerSession* handle = session.get();

    const AnalyzerMeasureConfig measure = analyzer_measure_config_default();
    AnalyzerStatus status = status_ok();
    ASSERT_TRUE(analyzer_session_start_measurement(handle, &measure, &status));
    analyzer_session_cancel_measurement(handle);

    AnalyzerMeasureProgress progress{};
    ASSERT_TRUE(analyzer_session_measure_progress(handle, &progress));
    EXPECT_FALSE(progress.active);
    EXPECT_EQ(progress.total, 0u);

    ASSERT_EQ(offline(*session).pump(), 256u);
    EXPECT_EQ(peak_of(offline(*session).captured_output()), 0.0f);
    analyzer_session_stop(session.release());
}

// A sweep that would overflow the deconvolution transform is refused, and the
// gate and range are clamped rather than trusted.
TEST(StartSession, ASweepLongerThanTheBufferIsRefused) {
    // 40 seconds is only too long for a fast device: 192 kHz makes 7.7 million
    // frames against a limit of 4.2 million.
    audio::Source source = noise(512, 2);
    source.sample_rate = 192'000.0;
    audio::OfflineBackend backend(std::move(source), 256);
    std::unique_ptr<AnalyzerSession> session = start_session(quiet_config(), std::nullopt, backend);

    AnalyzerMeasureConfig measure = analyzer_measure_config_default();
    measure.seconds = 30.0f;
    measure.tail_seconds = 10.0f;
    AnalyzerStatus status = status_ok();
    EXPECT_FALSE(analyzer_session_start_measurement(session.get(), &measure, &status));
    EXPECT_EQ(test::message_of(status), "that sweep is longer than the measurement buffer");
    analyzer_session_stop(session.release());
}

// ------------------------------------------------------------------ equaliser --

TEST(StartSession, TheEqualiserBoundsAndBandsBehaveAsDocumented) {
    audio::OfflineBackend backend(noise(512, 2), 256);
    std::unique_ptr<AnalyzerSession> session = start_session(quiet_config(), std::nullopt, backend);
    AnalyzerSession* handle = session.get();

    // Off: nothing to edit.
    AnalyzerBand band{AnalyzerFilterKind_Peaking, 100.0f, 3.0f, 2.0f, true};
    EXPECT_EQ(analyzer_session_eq_band_count(handle), 0u);
    EXPECT_EQ(analyzer_session_eq_add_band(handle, &band), -1);

    ASSERT_TRUE(analyzer_session_set_eq_mode(handle, AnalyzerEqMode_Graphic));
    EXPECT_EQ(analyzer_session_eq_band_count(handle), 10u);
    ASSERT_TRUE(analyzer_session_eq_set_gain(handle, 3, 4.0f));
    EXPECT_FALSE(analyzer_session_eq_set_gain(handle, 99, 4.0f));
    EXPECT_FALSE(analyzer_session_eq_set_gain(handle, 3, std::numeric_limits<float>::infinity()));

    ASSERT_TRUE(analyzer_session_set_eq_mode(handle, AnalyzerEqMode_Parametric));
    for (std::size_t i = 0; i < kMaxEqBands; ++i) {
        band.hz = 100.0f + static_cast<float>(i) * 50.0f;
        EXPECT_EQ(analyzer_session_eq_add_band(handle, &band), static_cast<std::intptr_t>(i));
    }
    // The audio thread's array is full; one more would silently not be heard.
    EXPECT_EQ(analyzer_session_eq_add_band(handle, &band), -1);
    band.q = std::numeric_limits<float>::quiet_NaN();
    EXPECT_FALSE(analyzer_session_eq_set_band(handle, 0, &band));

    AnalyzerEqInfo info{};
    ASSERT_TRUE(analyzer_session_eq_info(handle, &info));
    EXPECT_TRUE(info.active);
    EXPECT_EQ(info.band_count, kMaxEqBands);
    EXPECT_GT(info.peak_gain_db, 3.0f) << "bands add";

    ASSERT_TRUE(analyzer_session_eq_remove_band(handle, 0));
    EXPECT_EQ(analyzer_session_eq_band_count(handle), kMaxEqBands - 1);
    EXPECT_TRUE(analyzer_session_eq_trim(handle));
    EXPECT_TRUE(analyzer_session_eq_flatten(handle));

    // Both equalisers survive a mode switch.
    ASSERT_TRUE(analyzer_session_set_eq_mode(handle, AnalyzerEqMode_Graphic));
    AnalyzerBand read{};
    ASSERT_TRUE(analyzer_session_eq_get_band(handle, 3, &read));
    EXPECT_EQ(analyzer_session_eq_band_count(handle), 10u);

    // Curves are arithmetic, available without a single frame of audio.
    ASSERT_TRUE(analyzer_session_set_plot(handle, 100.0f, 100.0f, 20.0f, 20'000.0f, -120.0f, 0.0f,
                                          AnalyzerReduction_Max));
    float curve[100] = {};
    EXPECT_EQ(analyzer_session_copy_eq_curve(handle, -1, curve, 100), 100u);
    EXPECT_EQ(analyzer_session_copy_eq_curve(handle, 3, curve, 100), 100u);
    analyzer_session_stop(session.release());
}

}  // namespace
}  // namespace analyzer::ffi
