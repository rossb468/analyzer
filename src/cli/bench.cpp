#include "cli/bench.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "base/numeric.hpp"
#include "cli/text.hpp"
#include "dsp/generator.hpp"
#include "dsp/meter.hpp"
#include "dsp/octave.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/transfer.hpp"
#include "dsp/window.hpp"
#include "engine/ring.hpp"
#include "engine/rt.hpp"

namespace analyzer::cli {

namespace {

using Clock = std::chrono::steady_clock;

constexpr float kRate = 48'000.0f;

// Duty cycle the plan asks the analysis chain to stay under.
constexpr double kDutyTarget = 0.5;

// The FFT sizes a user can pick, for the two analysers.
constexpr std::array<std::size_t, 6> kSpectrumSizes{1024, 4096, 16'384, 32'768, 65'536, 131'072};
constexpr std::array<std::size_t, 4> kTransferSizes{4096, 16'384, 32'768, 65'536};

// Wants at least this many completed frames before a duty cycle is worth
// printing.
constexpr double kMinimumFrames = 8.0;

double seconds_since(Clock::time_point start) {
    return std::chrono::duration<double>(Clock::now() - start).count();
}

void line(std::string& out, const std::string& text = "") {
    out += text;
    out += '\n';
}

std::string verdict(double duty) {
    return duty < kDutyTarget ? "PASS" : "FAIL";
}

std::vector<float> audio(std::size_t samples, std::uint64_t seed) {
    dsp::Generator generator(kRate, dsp::Signal::pink_noise(0.5f), seed);
    std::vector<float> buffer(samples, 0.0f);
    generator.fill(buffer);
    return buffer;
}

// Whether this run is too short for the size to mean anything.
std::optional<std::string> too_short(std::size_t size, dsp::Overlap overlap, double seconds) {
    const auto hop = static_cast<double>(dsp::overlap_hop(overlap, size));
    const double frames = (static_cast<double>(kRate) * seconds) / hop;
    if (frames >= kMinimumFrames) {
        return std::nullopt;
    }
    const double needed = kMinimumFrames * hop / static_cast<double>(kRate);
    return "skipped - only " + text::fixed(frames, 1) + " frames in " + text::fixed(seconds, 1) +
           " s; needs --bench " + text::fixed(std::ceil(needed), 0);
}

std::string table_header() {
    return "  " + text::pad_left("fft", 7) + "  " + text::pad_left("frames/s", 9) + "  " +
           text::pad_left("duty", 10) + "  " + text::pad_left("realtime", 8) + "  " +
           text::pad_left("verdict", 7);
}

std::string table_row(std::size_t size, double frames_per_second, double duty) {
    return "  " + text::pad_left(std::to_string(size), 7) + "  " +
           text::pad_left(text::fixed(frames_per_second, 1), 9) + "  " +
           text::pad_left(text::fixed(duty, 4), 10) + "  " +
           text::pad_left(text::fixed(1.0 / duty, 0) + "x", 8) + "  " +
           text::pad_left(verdict(duty), 7);
}

// How many 120 Hz display reads fit in `seconds`: a UI reads at up to that
// rate, so the cost is included.
std::size_t display_reads(double seconds) {
    return saturating_cast<std::size_t>(seconds * 120.0);
}

// Duty cycle of the spectrum chain across the FFT sizes a user can pick.
void spectrum_duty(std::string& out, double seconds) {
    line(out, "spectrum analysis, 75% overlap");
    line(out, table_header());

    const auto total = saturating_cast<std::size_t>(static_cast<double>(kRate) * seconds);
    const std::vector<float> signal = audio(total, 1);

    for (const std::size_t size : kSpectrumSizes) {
        // A duty cycle measured over two or three frames says nothing. At
        // 131072 points and 75% overlap the hop alone is 0.68 s, so a short run
        // reports a flatteringly low figure simply because almost no work
        // happened. Skip loudly rather than printing a number that looks like a
        // result.
        if (const auto note = too_short(size, dsp::Overlap::ThreeQuarters, seconds)) {
            line(out, "  " + text::pad_left(std::to_string(size), 7) + "  " + *note);
            continue;
        }
        dsp::SpectrumAnalyzer analyzer(dsp::SpectrumConfig{
            .sample_rate = kRate,
            .size = size,
            .window = dsp::WindowKind::hann(),
            .overlap = dsp::Overlap::ThreeQuarters,
            .averaging = dsp::Averaging::exponential(0.2f),
        });
        std::vector<float> db(analyzer.bins(), 0.0f);

        // Feed in realistic callback-sized blocks; one giant push would flatter
        // the result by amortising the per-call overhead away.
        const auto start = Clock::now();
        std::size_t frames = 0;
        for (std::size_t offset = 0; offset < signal.size(); offset += 512) {
            frames += analyzer.push(std::span(signal).subspan(
                offset, std::min<std::size_t>(512, signal.size() - offset)));
        }
        for (std::size_t read = 0; read < display_reads(seconds); ++read) {
            analyzer.write_db_fs(db);
        }
        const double elapsed = seconds_since(start);

        const double duty = elapsed / seconds;
        line(out, table_row(size, static_cast<double>(frames) / seconds, duty));
    }
    line(out);
}

// The transfer function does two FFTs per frame plus the cross-spectrum, so it
// is the most expensive thing in Milestone 1.
void transfer_duty(std::string& out, double seconds) {
    line(out, "transfer function, two channels, 75% overlap");
    line(out, table_header());

    const auto total = saturating_cast<std::size_t>(static_cast<double>(kRate) * seconds);
    const std::vector<float> reference = audio(total, 2);
    const std::vector<float> measurement = audio(total, 3);

    for (const std::size_t size : kTransferSizes) {
        if (const auto note = too_short(size, dsp::Overlap::ThreeQuarters, seconds)) {
            line(out, "  " + text::pad_left(std::to_string(size), 7) + "  " + *note);
            continue;
        }
        dsp::TransferFunction tf(dsp::TransferConfig{
            .sample_rate = kRate,
            .size = size,
            .window = dsp::WindowKind::hann(),
            .overlap = dsp::Overlap::ThreeQuarters,
            .averaging = dsp::TransferAveraging::exponential(0.2f),
        });
        std::vector<float> magnitude(tf.bins(), 0.0f);
        std::vector<float> phase(tf.bins(), 0.0f);
        std::vector<float> coherence(tf.bins(), 0.0f);

        const auto start = Clock::now();
        std::size_t frames = 0;
        for (std::size_t offset = 0; offset < reference.size(); offset += 512) {
            const std::size_t count = std::min<std::size_t>(512, reference.size() - offset);
            frames += tf.push(std::span(reference).subspan(offset, count),
                              std::span(measurement).subspan(offset, count));
        }
        for (std::size_t read = 0; read < display_reads(seconds); ++read) {
            tf.write_magnitude_db(magnitude);
            tf.write_phase_degrees(phase);
            tf.write_coherence(coherence);
        }
        const double elapsed = seconds_since(start);

        const double duty = elapsed / seconds;
        line(out, table_row(size, static_cast<double>(frames) / seconds, duty));
    }
    line(out);
}

void meter_duty(std::string& out, double seconds) {
    line(out, "level meters, per-sample filtering");
    const auto total = saturating_cast<std::size_t>(static_cast<double>(kRate) * seconds);
    const std::vector<float> signal = audio(total, 4);

    for (const auto weighting :
         {dsp::MeterWeighting::Z, dsp::MeterWeighting::A, dsp::MeterWeighting::C}) {
        dsp::LevelMeter meter(kRate, weighting, dsp::Integration::fast());
        const auto start = Clock::now();
        for (std::size_t offset = 0; offset < signal.size(); offset += 512) {
            meter.push(std::span(signal).subspan(
                offset, std::min<std::size_t>(512, signal.size() - offset)));
        }
        const double duty = seconds_since(start) / seconds;
        // Rust's derived Debug writes the name without honouring the `{:>7?}`
        // width it was printed with, so the name is left unpadded to match.
        line(out, "  " + std::string(dsp::to_string(weighting)) + "  " +
                      text::pad_left(text::fixed(duty, 5), 10) + "  " +
                      text::pad_left(text::fixed(1.0 / duty, 0) + "x", 8) + "  " +
                      text::pad_left(verdict(duty), 7));
    }
    line(out);
}

void octave_duty(std::string& out, double seconds) {
    line(out, "octave banding, applied at 120 Hz");
    const std::vector<float> bins(4097, -60.0f);
    const float spacing = kRate / 8192.0f;

    for (const std::uint32_t fraction : {1u, 3u, 12u, 48u}) {
        const dsp::OctaveBands bands(fraction, 20.0f, 20'000.0f);
        std::vector<float> levels(bands.size(), 0.0f);

        const auto start = Clock::now();
        const std::size_t iterations = display_reads(seconds);
        for (std::size_t i = 0; i < iterations; ++i) {
            bands.apply(bins, spacing, levels);
        }
        const double duty = seconds_since(start) / seconds;
        line(out, "  1/" + text::pad_right(std::to_string(fraction), 5) + " " +
                      text::pad_left(std::to_string(bands.size()), 4) + " bands  " +
                      text::pad_left(text::fixed(duty, 5), 10) + "  " +
                      text::pad_left(verdict(duty), 7));
    }
    line(out);
}

// Push audio through the ring at wall-clock rate and count what gets dropped.
//
// This is the target that matters most, because an overrun is not a slow frame
// - it is a hole in the data that makes the measurement wrong.
void ring_soak(std::string& out, double seconds) {
    line(out, "ring soak, 128-frame blocks at wall-clock rate");

    const std::size_t block_frames = 128;
    auto ring = engine::capture_ring(2, 8192);
    auto& sink = ring.first;
    auto& source = ring.second;
    const auto blocks = saturating_cast<std::size_t>((static_cast<double>(kRate) * seconds) /
                                                     static_cast<double>(block_frames));

    dsp::SpectrumAnalyzer analyzer(dsp::SpectrumConfig{
        .sample_rate = kRate,
        .size = 8192,
        .window = dsp::WindowKind::hann(),
        .overlap = dsp::Overlap::ThreeQuarters,
        .averaging = dsp::Averaging::exponential(0.2f),
    });

    const std::vector<float> block = audio(block_frames * 2, 5);
    std::vector<float> interleaved(4096 * 2, 0.0f);
    std::vector<float> mono(4096, 0.0f);

    Clock::duration worst_callback = Clock::duration::zero();
    const auto start = Clock::now();

    for (std::size_t i = 0; i < blocks; ++i) {
        const auto callback_start = Clock::now();
        // Exactly what the audio thread does, inside the same guard.
        engine::rt_section([&] { static_cast<void>(sink.write_interleaved(block)); });
        worst_callback = std::max(worst_callback, Clock::now() - callback_start);

        // And exactly what the analysis thread does.
        const std::size_t frames = source.read_interleaved(interleaved);
        if (frames > 0) {
            for (std::size_t frame = 0; frame < frames; ++frame) {
                mono[frame] = interleaved[frame * 2];
            }
            analyzer.push(std::span<const float>(mono).first(frames));
        }
    }

    const double elapsed = seconds_since(start);
    const double budget_micros =
        static_cast<double>(block_frames) / static_cast<double>(kRate) * 1e6;

    line(out, "  blocks:          " + std::to_string(blocks));
    line(out, "  overruns:        " + std::to_string(sink.overruns()));
    line(out, "  worst callback:  " +
                  text::fixed(std::chrono::duration<double>(worst_callback).count() * 1e6, 1) +
                  " us (budget " + text::fixed(budget_micros, 0) + " us)");
    line(out, "  total duty:      " + text::fixed(elapsed / seconds, 4));
    line(out, std::string("  verdict:         ") + (sink.overruns() == 0 ? "PASS" : "FAIL"));
}

}  // namespace

std::string run_bench(double seconds) {
    std::string out;
    line(out, "analyzer benchmarks");
    line(out, "  sample rate: " + text::display(kRate) + " Hz");
    line(out, "  audio per case: " + text::fixed(seconds, 1) + " s");
    line(out, "  duty cycle = CPU seconds per second of audio; target < " +
                  text::display(kDutyTarget) + "\n");

    spectrum_duty(out, seconds);
    transfer_duty(out, seconds);
    meter_duty(out, seconds);
    octave_duty(out, seconds);
    ring_soak(out, seconds);

    return out;
}

}  // namespace analyzer::cli
