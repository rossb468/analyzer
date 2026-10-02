// Ported from crates/analyzer-engine/src/engine.rs.
//
// These drive a real worker thread, so they wait on it with deadlines rather
// than assuming how fast it runs.

#include "engine/engine.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <numeric>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::engine {
namespace {

using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

constexpr float kRate = 48'000.0f;
constexpr std::size_t kSize = 4096;
// Bin 85 exactly, so there is no scalloping loss to reason about.
constexpr float kOnBinHz = 996.09375f;

EngineConfig config(std::size_t channels, std::size_t analysis_channel) {
    EngineConfig c;
    c.channels = channels;
    c.analysis_channel = analysis_channel;
    c.spectrum = dsp::SpectrumConfig{
        .sample_rate = kRate,
        .size = kSize,
        .window = dsp::WindowKind::hann(),
        .overlap = dsp::Overlap::None,
        .averaging = dsp::Averaging::infinite(),
    };
    c.average = dsp::Averaging::infinite();
    c.mode = AnalysisMode::spectrum();
    c.ring_capacity_frames = 16'384;
    return c;
}

EngineConfig transfer_config() {
    auto c = config(2, 1);
    c.mode = AnalysisMode::transfer(0, 1);
    return c;
}

// Reproducible broadband noise. A transfer function needs energy in every
// bin, which a tone by definition does not provide.
std::vector<float> noise(std::size_t frames, std::uint64_t seed) {
    std::uint64_t state = seed | 1;
    std::vector<float> out(frames);
    for (auto& sample : out) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        sample = static_cast<float>(state >> 40) / 8'388'608.0f - 1.0f;
    }
    return out;
}

// A continuous two-channel stream: a reference, and the same signal scaled and
// delayed.
//
// Continuous rather than one block replayed, and that is not fussiness. A
// repeated block is periodic, and cross-correlation genuinely cannot tell a
// delay of d from a delay of d minus the period - the first version of this
// helper repeated a 4096-frame block and the delay finder correctly reported
// -3968 for a 128-sample delay.
class Echo {
public:
    Echo(std::size_t blocks, std::size_t block, float gain, std::size_t delay)
        : data_(blocks * block * 2, 0.0f), block_(block) {
        const std::size_t frames = blocks * block;
        const auto source = noise(frames + delay, 0x1234'5678);
        for (std::size_t frame = 0; frame < frames; ++frame) {
            data_[frame * 2] = source[frame + delay];
            data_[frame * 2 + 1] = gain * source[frame];
        }
    }

    // The next block, wrapping once the stream is exhausted.
    std::span<const float> next_block() {
        const std::size_t stride = block_ * 2;
        if (cursor_ + stride > data_.size()) {
            cursor_ = 0;
        }
        const std::size_t at = cursor_;
        cursor_ += stride;
        return std::span<const float>(data_).subspan(at, stride);
    }

private:
    std::vector<float> data_;
    std::size_t block_;
    std::size_t cursor_ = 0;
};

// Pump until the engine has folded in `want` transfer frames.
SpectrumFrame pump_transfer(CaptureSink& sink, Engine& engine, Echo& echo, std::uint32_t want) {
    for (int i = 0; i < 400; ++i) {
        sink.write_interleaved(echo.next_block());
        std::this_thread::sleep_for(2ms);
        if (engine.latest().transfer_frames >= want) {
            break;
        }
    }
    return engine.latest();
}

// Interleaved frames with a tone on `tone_channel` and silence elsewhere.
std::vector<float> tone(std::size_t frames, std::size_t channels, std::size_t tone_channel,
                        float amplitude) {
    std::vector<float> out(frames * channels, 0.0f);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        const float phase =
            2.0f * std::numbers::pi_v<float> * kOnBinHz * static_cast<float>(frame) / kRate;
        out[frame * channels + tone_channel] = amplitude * std::sin(phase);
    }
    return out;
}

// Feed blocks until the engine has published, or fail.
void feed_until_published(CaptureSink& sink, const Engine& engine, std::span<const float> samples,
                          std::size_t block) {
    const auto deadline = Clock::now() + 5s;
    std::size_t offset = 0;
    while (engine.published_count() == 0) {
        ASSERT_LT(Clock::now(), deadline) << "engine never published";
        if (offset >= samples.size()) {
            offset = 0;
        }
        const std::size_t end = std::min(offset + block, samples.size());
        // Respect backpressure rather than flooding. A real audio callback
        // produces at wall-clock rate into a ring sized to absorb it; a test
        // that hammers as fast as it can would overrun by construction and say
        // nothing useful about the engine.
        const std::size_t wanted = (end - offset) / sink.channels();
        if (sink.frames_free() >= wanted &&
            sink.write_interleaved(samples.subspan(offset, end - offset))) {
            offset = end;
        } else {
            std::this_thread::sleep_for(1ms);
        }
    }
}

std::size_t loudest_bin(const std::vector<float>& bins) {
    return static_cast<std::size_t>(std::max_element(bins.begin(), bins.end()) - bins.begin());
}

// A known gain between two channels must come back as that gain.
TEST(Engine, ATransferFunctionRecoversAKnownGain) {
    auto [sink, engine] = Engine::start(transfer_config());
    Echo echo(16, 4096, 0.5f, 0);
    const auto frame = pump_transfer(sink, engine, echo, 8);
    engine.stop();

    ASSERT_GE(frame.transfer_frames, 8u) << "no transfer frames were produced";
    ASSERT_EQ(frame.transfer_magnitude_db.size(), kSize / 2 + 1);

    // Skip the extremes: the lowest bins hold too little noise energy to
    // settle, and the topmost bin is a half-bin special case.
    const auto first = frame.transfer_magnitude_db.begin() + 20;
    const auto last = frame.transfer_magnitude_db.begin() + 1800;
    const float mean = std::accumulate(first, last, 0.0f) / static_cast<float>(last - first);
    EXPECT_NEAR(mean, -6.02f, 0.5f) << "half amplitude should read about -6 dB";

    const float worst = *std::min_element(frame.transfer_coherence.begin() + 20,
                                          frame.transfer_coherence.begin() + 1800);
    EXPECT_GT(worst, 0.99f) << "a noiseless path should be fully coherent";
}

// A pure delay must show up as coherent but phase-wound, and compensating it
// must flatten the phase back out.
TEST(Engine, CompensatingAKnownDelayFlattensThePhase) {
    constexpr std::size_t delay = 64;
    auto [sink, engine] = Engine::start(transfer_config());
    Echo echo(16, 4096, 1.0f, delay);

    engine.set_reference_delay(delay);
    const auto frame = pump_transfer(sink, engine, echo, 8);
    engine.stop();

    ASSERT_GE(frame.transfer_frames, 8u);
    EXPECT_EQ(frame.transfer_delay_frames, delay);

    float worst = 0.0f;
    for (std::size_t bin = 20; bin < 1800; ++bin) {
        worst = std::max(worst, std::abs(frame.transfer_phase_degrees[bin]));
    }
    EXPECT_LT(worst, 5.0f) << "compensated phase should be flat";
}

// The finder must recover a delay nobody told it about.
TEST(Engine, TheEngineCanMeasureTheDelayItself) {
    constexpr std::uint32_t delay = 128;
    auto [sink, engine] = Engine::start(transfer_config());
    Echo echo(16, 4096, 1.0f, delay);

    engine.estimate_reference_delay();
    pump_transfer(sink, engine, echo, 8);
    const std::uint32_t found = engine.reference_delay();
    engine.stop();

    EXPECT_LE(found > delay ? found - delay : delay - found, 1u)
        << "expected about " << delay << " samples of delay, found " << found;
}

// Spectrum mode must not publish a stale or empty transfer curve that a UI
// could mistake for a real one.
TEST(Engine, SpectrumModePublishesNoTransferCurves) {
    auto [sink, engine] = Engine::start(config(1, 0));
    const auto data = tone(4096, 1, 0, 0.5f);
    for (int i = 0; i < 40; ++i) {
        sink.write_interleaved(data);
        std::this_thread::sleep_for(2ms);
        if (engine.latest().frames_averaged > 0) {
            break;
        }
    }
    const auto frame = engine.latest();
    engine.stop();

    EXPECT_GT(frame.frames_averaged, 0u) << "the spectrum should still run";
    EXPECT_EQ(frame.transfer_frames, 0u);
    EXPECT_TRUE(frame.transfer_magnitude_db.empty());
    EXPECT_TRUE(frame.transfer_coherence.empty());
}

// In transfer mode the spectrum follows the measurement channel, so the level
// on screen is the level being measured.
TEST(Engine, TheSpectrumFollowsTheMeasurementChannelInTransferMode) {
    auto [sink, engine] = Engine::start(transfer_config());
    // Loud reference on channel 0, quiet measurement on channel 1.
    Echo echo(16, 4096, 0.01f, 0);
    const auto frame = pump_transfer(sink, engine, echo, 4);
    engine.stop();

    const float peak = *std::max_element(frame.bins.begin(), frame.bins.end());
    EXPECT_LT(peak, -40.0f) << "the spectrum showed the reference, not the measurement";
}

TEST(Engine, PublishesAFrameWithTheToneInTheRightBin) {
    auto [sink, engine] = Engine::start(config(1, 0));
    const auto samples = tone(kSize * 2, 1, 0, 0.5f);
    feed_until_published(sink, engine, samples, 128);

    const auto frame = engine.latest();
    EXPECT_GT(frame.sequence, 0u);
    ASSERT_EQ(frame.bins.size(), kSize / 2 + 1);
    EXPECT_EQ(frame.overruns, 0u);

    EXPECT_EQ(loudest_bin(frame.bins), 85u) << "tone should land in bin 85";
    EXPECT_NEAR(frame.bins[85], -6.0206f, 0.05f);
    EXPECT_NEAR(frame.bin_spacing_hz, kRate / static_cast<float>(kSize), 1e-3f);
    EXPECT_NEAR(frame.sample_rate, kRate, 0.5f);
}

TEST(Engine, AnalysesTheSelectedChannelOnly) {
    {
        // Tone on channel 1, silence on channel 0.
        auto [sink, engine] = Engine::start(config(2, 1));
        const auto samples = tone(kSize * 2, 2, 1, 0.5f);
        feed_until_published(sink, engine, samples, 128);
        EXPECT_NEAR(engine.latest().bins[85], -6.0206f, 0.05f);
    }
    {
        auto [sink, engine] = Engine::start(config(2, 0));
        const auto samples = tone(kSize * 2, 2, 1, 0.5f);
        feed_until_published(sink, engine, samples, 128);
        EXPECT_LT(engine.latest().bins[85], -80.0f) << "silent channel picked up the tone";
    }
}

// Both traces must describe the same audio, differing only in how they
// average. A steady tone settles to the same answer either way.
TEST(Engine, TheAverageTraceTracksTheSameSignal) {
    auto [sink, engine] = Engine::start(config(1, 0));
    const auto samples = tone(kSize * 8, 1, 0, 0.5f);
    feed_until_published(sink, engine, samples, 256);

    // Give the average a few frames to settle.
    const auto deadline = Clock::now() + 5s;
    while (engine.published_count() < 4 && Clock::now() < deadline) {
        sink.write_interleaved(std::span<const float>(samples).first(256));
        std::this_thread::sleep_for(1ms);
    }

    const auto frame = engine.latest();
    EXPECT_EQ(frame.average_bins.size(), frame.bins.size()) << "both traces span the same bins";
    EXPECT_GT(frame.average_frames, 0u) << "the average should have run";
    EXPECT_NEAR(frame.average_bins[85], frame.bins[85], 1.0f)
        << "on a steady tone they should agree";
}

// Resetting the average must not disturb the live trace, which is the whole
// point of running two analyzers.
TEST(Engine, ResettingTheAverageLeavesTheLiveTraceAlone) {
    auto [sink, engine] = Engine::start(config(1, 0));
    const auto samples = tone(kSize * 4, 1, 0, 0.5f);
    feed_until_published(sink, engine, samples, 256);

    const auto before = engine.latest();
    ASSERT_GT(before.average_frames, 0u);

    engine.reset_average();

    // Feed enough for the worker to see the flag and publish again, as a
    // continuous stream rather than the same block over and over.
    //
    // Re-sending samples[0..256) would not be the tone: 256 samples is 5.3125
    // cycles at this frequency, so repeating it restarts the phase every block
    // and the discontinuity smears energy off bin 85. The live trace would
    // then genuinely move, and whether this test passed would depend on how
    // many such frames landed before latest() was read. The whole buffer is
    // exactly 340 cycles, so wrapping it is seamless.
    const auto deadline = Clock::now() + 5s;
    const auto target = engine.published_count() + 2;
    std::size_t offset = 0;
    const std::span<const float> stream(samples);
    while (engine.published_count() < target && Clock::now() < deadline) {
        if (offset >= stream.size()) {
            offset = 0;
        }
        const std::size_t end = std::min(offset + 256, stream.size());
        if (sink.write_interleaved(stream.subspan(offset, end - offset))) {
            offset = end;
        }
        std::this_thread::sleep_for(1ms);
    }

    const auto after = engine.latest();
    EXPECT_LT(after.average_frames, before.average_frames + 2)
        << "the average should have restarted: " << before.average_frames << " then "
        << after.average_frames;
    EXPECT_NEAR(after.bins[85], before.bins[85], 1.0f) << "the live trace must be undisturbed";
}

TEST(Engine, HasNewFrameTracksPublication) {
    auto [sink, engine] = Engine::start(config(1, 0));
    const auto samples = tone(kSize * 2, 1, 0, 0.5f);
    feed_until_published(sink, engine, samples, 256);

    EXPECT_TRUE(engine.has_new_frame());
    engine.latest();
    EXPECT_FALSE(engine.has_new_frame()) << "consumed frame should clear";
}

TEST(Engine, ReportsOverrunsFromAStarvedAnalysisThread) {
    auto c = config(1, 0);
    // Tiny ring, so filling it faster than the worker drains is easy.
    c.ring_capacity_frames = 256;
    auto [sink, engine] = Engine::start(c);

    const std::vector<float> block(256, 0.0f);
    std::uint64_t refused = 0;
    for (int i = 0; i < 2'000; ++i) {
        if (!sink.write_interleaved(block)) {
            ++refused;
        }
    }
    EXPECT_GT(refused, 0u) << "should have overrun a 256-frame ring";
    EXPECT_GE(sink.overruns(), refused);
}

// Regression test. An earlier version drained the ring until it was empty
// before publishing, which looks natural and is wrong: a producer that keeps
// the ring topped up means the drain loop never exits and nothing is ever
// published. This floods as hard as it can and still expects progress.
TEST(Engine, AFastProducerCannotStarvePublication) {
    auto c = config(1, 0);
    c.ring_capacity_frames = 32'768;
    auto [sink, engine] = Engine::start(c);

    const std::vector<float> block(1024, 0.25f);
    const auto deadline = Clock::now() + 5s;
    while (engine.published_count() == 0) {
        ASSERT_LT(Clock::now(), deadline)
            << "publication starved by a producer that keeps the ring full";
        // Deliberately unpaced, refusals ignored: the harshest case.
        sink.write_interleaved(block);
    }
}

TEST(Engine, StopIsIdempotentAndDestructionJoins) {
    auto [sink, engine] = Engine::start(config(1, 0));
    engine.stop();
    engine.stop();
    // Destruction must not hang or double-join.
}

// The C++ addition: moving an engine hands over the worker rather than
// stopping it, and the moved-from shell destructs quietly.
TEST(Engine, MovingAnEngineKeepsItsWorker) {
    auto [sink, started] = Engine::start(config(1, 0));
    Engine moved = std::move(started);
    const auto samples = tone(kSize * 2, 1, 0, 0.5f);
    feed_until_published(sink, moved, samples, 256);
    EXPECT_GT(moved.published_count(), 0u);
}

TEST(Engine, IdleEnginePublishesNothing) {
    auto [sink, engine] = Engine::start(config(1, 0));
    std::this_thread::sleep_for(50ms);
    EXPECT_EQ(engine.published_count(), 0u) << "an engine with no input must not publish";
}

TEST(EngineDeathTest, RejectsAnAnalysisChannelThatDoesNotExist) {
    EXPECT_DEATH(Engine::start(config(2, 2)), "analysis channel does not exist");
}

TEST(EngineDeathTest, RejectsTransferAgainstItself) {
    auto c = config(2, 0);
    c.mode = AnalysisMode::transfer(1, 1);
    EXPECT_DEATH(Engine::start(c), "is a wire, not a measurement");
}

// -------------------------------------------------------------- recording -

// Swept measurement needs the samples themselves. The capture has to fill with
// what was actually played, and stop at exactly the length asked for.
TEST(EngineRecording, CapturesTheRequestedLengthAndNoMore) {
    auto [sink, engine] = Engine::start(config(1, 0));
    engine.begin_recording(1000);

    // A ramp, so a short or misaligned capture is visible rather than
    // plausible: sample n has value n.
    std::vector<float> block(4096);
    std::iota(block.begin(), block.end(), 0.0f);
    // The ring is smaller than the block, so feed it in chunks and let the
    // analysis thread drain between them.
    const auto deadline = Clock::now() + 5s;
    std::size_t written = 0;
    while (written < block.size() && Clock::now() < deadline) {
        const std::size_t end = std::min(written + 512, block.size());
        if (sink.write_interleaved(std::span<const float>(block).subspan(written, end - written))) {
            written = end;
        }
        std::this_thread::sleep_for(1ms);
    }

    std::optional<std::vector<float>> captured;
    while (Clock::now() < deadline && !captured) {
        captured = engine.take_recording();
        std::this_thread::sleep_for(1ms);
    }

    ASSERT_TRUE(captured.has_value()) << "recording never completed";
    ASSERT_EQ(captured->size(), 1000u) << "captured the wrong length";
    EXPECT_EQ((*captured)[0], 0.0f);
    EXPECT_EQ((*captured)[999], 999.0f) << "samples arrived out of order";
}

// Polling must not hand back half a sweep, which would deconvolve into a
// measurement that looks real and is not.
TEST(EngineRecording, AnIncompleteRecordingIsNotHandedOver) {
    auto [sink, engine] = Engine::start(config(1, 0));
    engine.begin_recording(48'000);
    EXPECT_FALSE(engine.take_recording().has_value());
    EXPECT_EQ(engine.recording_progress(), (RecordingProgress{0, 48'000}));
}

TEST(EngineRecording, CancellingReleasesTheCapture) {
    auto [sink, engine] = Engine::start(config(1, 0));
    engine.begin_recording(48'000);
    engine.cancel_recording();
    EXPECT_EQ(engine.recording_progress(), (RecordingProgress{0, 0}));
    EXPECT_FALSE(engine.take_recording().has_value());
}

}  // namespace
}  // namespace analyzer::engine
