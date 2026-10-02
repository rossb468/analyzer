// Live capture through CoreAudio.
//
// Split from live.hpp's other implementation so the portable half of the
// harness - WAV analysis, the bench, swept measurement - compiles on Linux and
// Windows, where this file is not built at all.
//
// This exists so the audio backend can be proven from a terminal, before any
// user interface is written. When the GUI later fails to show a spectrum, that
// failure is unambiguously the GUI's - the chain from converter to analysis is
// already known good.
//
// The file asks audio::default_backend() for its backend rather than naming
// CoreAudio: nothing outside the audio module does. It is compiled only on
// macOS because that is the only platform where the backend it gets can
// capture.
//
// Microphone permission
// ---------------------
// macOS gates capture behind TCC. A bare binary run from a terminal inherits
// the terminal's permission and the first attempt prompts the user. If
// permission is denied, capture still "succeeds" and simply delivers digital
// silence forever, so this file watches for that and says so rather than
// leaving the user staring at an empty meter.

#include <algorithm>
#include <chrono>
#include <exception>
#include <future>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "audio/error.hpp"
#include "audio/platform.hpp"
#include "base/number_text.hpp"
#include "cli/error.hpp"
#include "cli/live.hpp"
#include "cli/live_display.hpp"
#include "engine/rt.hpp"

namespace analyzer::cli {

namespace {

using Clock = std::chrono::steady_clock;

// Run `step`, turning an audio failure into the harness's own error with the
// step's name in front, the way the Rust mapped each error with a format string.
template <class F>
decltype(auto) context(const std::string& what, F&& step) {
    try {
        return std::forward<F>(step)();
    } catch (const audio::AudioError& error) {
        throw CliError(what + ": " + error.what());
    }
}

engine::SpectrumFrame capture_inner(const LiveOptions& options) {
    const std::unique_ptr<audio::AudioBackend> backend = audio::default_backend();

    const audio::DeviceInfo device = [&] {
        if (options.device) {
            const std::vector<audio::DeviceInfo> devices =
                context("enumerating devices", [&] { return backend->devices(); });
            const auto found = std::ranges::find_if(
                devices, [&](const audio::DeviceInfo& d) { return d.id.str() == *options.device; });
            if (found == devices.end()) {
                throw CliError("no device with uid '" + *options.device + "' (try --list-devices)");
            }
            return *found;
        }
        const std::optional<audio::DeviceInfo> input =
            context("finding the default input", [&] { return backend->default_input(); });
        if (!input) {
            throw CliError("no default input device");
        }
        return *input;
    }();

    if (device.input_channels == 0) {
        throw CliError(device.name + " has no input channels");
    }
    if (options.channel >= device.input_channels) {
        throw CliError("channel " + std::to_string(options.channel) + " requested but " +
                       device.name + " has " + std::to_string(device.input_channels));
    }

    // Capture every channel and pick one for analysis, rather than asking the
    // device for a single channel. Interleaved capture of the whole device is
    // what a two-channel transfer function will need later.
    const std::size_t channels = device.input_channels;
    const double rate = device.default_sample_rate;

    dsp::SpectrumConfig spectrum = options.spectrum;
    spectrum.sample_rate = static_cast<float>(rate);

    auto started = engine::Engine::start(engine::EngineConfig{
        .channels = channels,
        .analysis_channel = options.channel,
        .spectrum = spectrum,
        // The CLI prints one settled figure rather than a live curve, so the
        // long-term trace is not used here; it still has to be configured.
        .average = dsp::Averaging::infinite(),
        // The CLI's live mode is a single-channel meter; the transfer function
        // has its own path.
        .mode = engine::AnalysisMode::spectrum(),
        .ring_capacity_frames = 16'384,
    });
    engine::Engine& engine = started.second;

    audio::StreamConfig config;
    config.input = audio::DeviceId(device.id.str());
    config.sample_rate = rate;
    config.buffer_frames = options.block;
    for (std::size_t channel = 0; channel < channels; ++channel) {
        config.input_channels.push_back(static_cast<std::uint32_t>(channel));
    }

    const std::unique_ptr<audio::AudioStream> stream = context("opening " + device.name, [&] {
        return backend->open(config,
                             // The real-time path, guarded. If this ever allocates the process
                             // aborts rather than glitching.
                             audio::make_callback([sink = std::move(started.first)](
                                                      audio::AudioBuffers& buffers) mutable {
                                 engine::rt_section([&] {
                                     static_cast<void>(sink.write_interleaved(buffers.input()));
                                 });
                             }));
    });

    const audio::StreamConfig granted = stream->config();
    const audio::StreamLatency latency = stream->latency();

    std::cerr << "capturing from " << device.name << " (" << channels << " ch @ "
              << text::shortest(granted.sample_rate) << " Hz, " << granted.buffer_frames
              << " frame buffer, analysing channel " << options.channel << ")\n";
    std::cerr << "hardware latency: " << latency.input_frames << " frames in, "
              << latency.safety_offset_frames << " safety ("
              << text::fixed(latency.round_trip_seconds(granted.sample_rate) * 1000.0, 2)
              << " ms round trip)\n";

    context("starting", [&] { stream->start(); });

    const auto deadline = Clock::now() + std::chrono::duration_cast<Clock::duration>(
                                             std::chrono::duration<double>(options.seconds));
    auto last_meter = Clock::now();
    float loudest = -std::numeric_limits<float>::infinity();

    while (Clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        if (!engine.has_new_frame()) {
            continue;
        }
        const engine::SpectrumFrame& frame = engine.latest();
        const float level = broadband_db(frame.bins);
        loudest = std::max(loudest, level);

        if (options.meter && Clock::now() - last_meter >= std::chrono::milliseconds(100)) {
            last_meter = Clock::now();
            const auto [bin, peak] = peak_bin(frame.bins);
            std::cerr << "\r  " << text::pad_left(text::fixed(level, 1), 7) << " dBFS "
                      << bar(level) << "  peak "
                      << text::pad_left(text::fixed(frame.bin_frequency(bin), 1), 8) << " Hz @ "
                      << text::pad_left(text::fixed(peak, 1), 6) << " dB   " << std::flush;
        }
    }

    context("stopping", [&] { stream->stop(); });
    if (options.meter) {
        std::cerr << '\n';
    }

    const engine::SpectrumFrame frame = engine.latest();

    if (frame.overruns > 0) {
        throw CliError(std::to_string(frame.overruns) +
                       " block(s) dropped during capture - the measurement is invalid");
    }
    if (frame.frames_averaged == 0) {
        throw CliError("no audio was captured at all");
    }
    if (loudest <= kSilenceDb) {
        throw CliError("captured only digital silence from " + device.name +
                       ".\n"
                       "A real microphone always has some self-noise, so this is almost certainly\n"
                       "macOS microphone permission being denied rather than a quiet room.\n"
                       "Check System Settings > Privacy & Security > Microphone for your "
                       "terminal.");
    }

    return frame;
}

}  // namespace

std::string list_devices() {
    const std::unique_ptr<audio::AudioBackend> backend = audio::default_backend();
    const std::vector<audio::DeviceInfo> devices =
        context("enumerating devices", [&] { return backend->devices(); });

    std::string out = "Audio devices\n";
    for (const audio::DeviceInfo& device : devices) {
        std::string tags;
        const auto tag = [&](const char* name) {
            tags += tags.empty() ? "" : ", ";
            tags += name;
        };
        if (device.is_default_input) {
            tag("default input");
        }
        if (device.is_default_output) {
            tag("default output");
        }

        out += "\n  " + device.name + (tags.empty() ? "" : "  [" + tags + "]") + "\n";
        out += "    uid:      " + device.id.str() + "\n";
        out += "    channels: " + std::to_string(device.input_channels) + " in, " +
               std::to_string(device.output_channels) + " out\n";
        out += "    rate:     " + text::shortest(device.default_sample_rate) + " Hz\n";
        if (!device.supported_sample_rates.empty()) {
            out += "    supports: ";
            for (std::size_t i = 0; i < device.supported_sample_rates.size(); ++i) {
                out += (i == 0 ? "" : ", ") + text::fixed(device.supported_sample_rates[i], 0);
            }
            out += "\n";
        }
    }
    return out;
}

// Runs the actual work on a thread behind a deadline. A refused microphone
// permission does not fail promptly - CoreAudio's server retries
// StartAndWaitForState on a 30 second timeout, so a denied stream stalls for
// minutes and looks like a hang. The deadline turns that into an explanation.
engine::SpectrumFrame capture(const LiveOptions& options) {
    auto result = std::make_shared<std::promise<engine::SpectrumFrame>>();
    std::future<engine::SpectrumFrame> future = result->get_future();

    // Detached: on a timeout the thread is still stuck inside CoreAudio and
    // cannot be joined. It owns its own copy of the options and a share of the
    // promise, so nothing it touches goes away under it.
    std::thread([result, owned = options] {
        try {
            result->set_value(capture_inner(owned));
        } catch (...) {
            result->set_exception(std::current_exception());
        }
    }).detach();

    // Enough slack for device negotiation, but far short of CoreAudio's retries.
    const double budget = options.seconds + 15.0;
    if (future.wait_for(std::chrono::duration<double>(budget)) != std::future_status::ready) {
        throw CliError("capture did not start within " + text::fixed(budget, 0) +
                       "s.\n"
                       "CoreAudio stalls like this when microphone access is refused, and macOS "
                       "will not raise a permission prompt for a process launched in a "
                       "non-interactive background session - it refuses silently.\n"
                       "Run this once from a foreground Terminal window and allow the prompt, or "
                       "grant access under System Settings > Privacy & Security > Microphone.");
    }
    return future.get();
}

}  // namespace analyzer::cli
