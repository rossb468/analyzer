// Starting and stopping a session.

#include <algorithm>
#include <stdexcept>
#include <string>

#include "audio/platform.hpp"
#include "dsp/spectrum.hpp"
#include "engine/engine.hpp"
#include "engine/snapshot.hpp"
#include "ffi/convert.hpp"
#include "ffi/internal.hpp"
#include "ffi/session.hpp"
#include "ffi/stimulus_callback.hpp"

namespace analyzer::ffi {

namespace {

using analyzer::audio::DeviceId;
using analyzer::audio::DeviceInfo;

DeviceInfo find_device(audio::AudioBackend& backend, const std::optional<std::string>& uid) {
    if (uid) {
        const std::vector<DeviceInfo> devices = backend.devices();
        const auto found = std::find_if(devices.begin(), devices.end(),
                                        [&](const DeviceInfo& d) { return d.id.str() == *uid; });
        if (found == devices.end()) {
            throw std::runtime_error("no device with uid '" + *uid + "'");
        }
        return *found;
    }
    std::optional<DeviceInfo> device = backend.default_input();
    if (!device) {
        throw std::runtime_error("no default input device");
    }
    return *std::move(device);
}

// The engine's view of what the session computes, validated against the device.
engine::AnalysisMode choose_mode(const AnalyzerSessionConfig& config, const DeviceInfo& device,
                                 std::size_t inputs, std::size_t channels, bool internal_reference,
                                 bool playing) {
    switch (config.mode) {
        case AnalyzerMode_Spectrum: return engine::AnalysisMode::spectrum();
        case AnalyzerMode_Transfer: break;
    }

    const std::size_t reference_channel =
        internal_reference ? inputs : static_cast<std::size_t>(config.reference_channel);
    if (reference_channel >= channels) {
        throw std::runtime_error("reference channel " + std::to_string(reference_channel) +
                                 " requested but " + device.name + " has " +
                                 std::to_string(inputs));
    }
    if (reference_channel == static_cast<std::size_t>(config.channel)) {
        throw std::runtime_error(
            "the reference and measurement channels must differ; the same channel "
            "against itself measures a wire, not a loudspeaker");
    }
    if (internal_reference && !playing) {
        throw std::runtime_error(
            "an internal reference needs a stimulus to reference; choose a signal "
            "or wire a loopback into a second input");
    }
    return engine::AnalysisMode::transfer(reference_channel,
                                          static_cast<std::size_t>(config.channel));
}

}  // namespace

std::unique_ptr<AnalyzerSession> start_session(const AnalyzerSessionConfig& config,
                                               const std::optional<std::string>& uid,
                                               audio::AudioBackend& backend) {
    if (config.fft_size < 2 || config.fft_size % 2 != 0) {
        throw std::runtime_error("fft size must be even and at least 2, got " +
                                 std::to_string(config.fft_size));
    }

    const DeviceInfo device = find_device(backend, uid);

    if (device.input_channels == 0) {
        throw std::runtime_error(device.name + " has no input channels");
    }
    if (config.channel >= device.input_channels) {
        throw std::runtime_error("channel " + std::to_string(config.channel) + " requested but " +
                                 device.name + " has " + std::to_string(device.input_channels));
    }

    const std::size_t inputs = device.input_channels;
    const double rate = device.default_sample_rate;
    // The analysis and the generator both divide by the rate; a device that
    // reports none cannot be measured, and is better refused than aborted on.
    if (!(rate > 0.0)) {
        throw std::runtime_error(device.name + " reports no sample rate");
    }

    const bool playing = config.signal != AnalyzerSignal_Silence;
    std::vector<std::uint32_t> output_channels;
    if (playing) {
        for (std::uint32_t c = 0; c < device.output_channels; ++c) {
            if (c < 32 && ((config.output_mask >> c) & 1u) != 0) {
                output_channels.push_back(c);
            }
        }
    }
    if (playing && output_channels.empty()) {
        throw std::runtime_error("a stimulus was requested but no output channel was selected; " +
                                 device.name + " has " + std::to_string(device.output_channels) +
                                 " output(s)");
    }

    // The internal reference is captured as one extra channel appended to the
    // device's own, so the stimulus travels through the same ring, in the same
    // block, as the audio it will be compared against. Nothing downstream can
    // then slide the two apart.
    const bool internal_reference =
        config.mode == AnalyzerMode_Transfer && config.reference == AnalyzerReference_Internal;
    const std::size_t channels = inputs + (internal_reference ? 1 : 0);

    const engine::AnalysisMode mode =
        choose_mode(config, device, inputs, channels, internal_reference, playing);

    const dsp::Overlap overlap = to_overlap(config.overlap);
    const std::size_t hop = dsp::overlap_hop(overlap, config.fft_size);
    const float sample_rate = static_cast<float>(rate);
    const float frames_per_second = sample_rate / static_cast<float>(hop);

    dsp::Averaging averaging = dsp::Averaging::none();
    switch (config.averaging) {
        case AnalyzerAveraging_None: averaging = dsp::Averaging::none(); break;
        case AnalyzerAveraging_Fast:
            averaging = dsp::Averaging::exponential_over(1.0f, frames_per_second);
            break;
        case AnalyzerAveraging_Infinite: averaging = dsp::Averaging::infinite(); break;
        case AnalyzerAveraging_PeakHold: averaging = dsp::Averaging::peak_hold(); break;
    }

    engine::EngineConfig engine_config;
    engine_config.channels = channels;
    engine_config.analysis_channel = config.channel;
    engine_config.spectrum.sample_rate = sample_rate;
    engine_config.spectrum.size = config.fft_size;
    engine_config.spectrum.window = to_window_kind(config.window);
    engine_config.spectrum.overlap = overlap;
    engine_config.spectrum.averaging = averaging;
    // The long-term trace always averages everything since its last reset.
    // Anything shorter would just be a second live trace.
    engine_config.average = dsp::Averaging::infinite();
    engine_config.mode = mode;
    engine_config.ring_capacity_frames = 16'384;

    auto [sink, running_engine] = engine::Engine::start(engine_config);

    audio::StreamConfig stream_config;
    stream_config.input = DeviceId(device.id.str());
    if (playing) {
        stream_config.output = DeviceId(device.id.str());
    }
    stream_config.sample_rate = rate;
    stream_config.buffer_frames = config.buffer_frames;
    for (std::uint32_t c = 0; c < device.input_channels; ++c) {
        stream_config.input_channels.push_back(c);
    }
    stream_config.output_channels = std::move(output_channels);

    auto signal =
        std::make_shared<SignalState>(config.signal, config.signal_level_db, config.signal_hz);

    auto [eq_publisher, eq_reader] = engine::snapshot_channel(EqCoefficients{});

    std::unique_ptr<audio::AudioStream> stream;
    try {
        stream = backend.open(stream_config,
                              std::make_unique<StimulusCallback>(
                                  std::move(sink), signal, EqProcessor(std::move(eq_reader)),
                                  sample_rate, channels, playing, internal_reference));
    } catch (const std::exception& error) {
        throw std::runtime_error("opening " + device.name + ": " + error.what());
    }

    try {
        stream->start();
    } catch (const std::exception& error) {
        throw std::runtime_error("starting " + device.name + ": " + error.what());
    }

    return std::make_unique<AnalyzerSession>(std::move(running_engine), std::move(stream),
                                             sample_rate, std::move(signal),
                                             std::move(eq_publisher), device.name);
}

}  // namespace analyzer::ffi

extern "C" AnalyzerSessionConfig analyzer_session_config_default(void) noexcept {
    AnalyzerSessionConfig config{};
    config.device_uid = nullptr;
    config.channel = 0;
    config.fft_size = 4096;
    config.buffer_frames = 512;
    config.window = AnalyzerWindow_Hann;
    config.overlap = AnalyzerOverlap_ThreeQuarters;
    config.averaging = AnalyzerAveraging_Fast;
    config.mode = AnalyzerMode_Spectrum;
    config.reference = AnalyzerReference_Internal;
    config.reference_channel = 1;
    config.signal = AnalyzerSignal_Silence;
    // Quiet enough not to startle anyone, loud enough to measure. A default
    // that plays at full scale into unknown speakers is a default that damages
    // something.
    config.signal_level_db = -20.0f;
    config.signal_hz = 1000.0f;
    config.output_mask = 0b11;
    return config;
}

extern "C" AnalyzerSession* analyzer_session_start(const AnalyzerSessionConfig* config,
                                                   AnalyzerStatus* status) noexcept {
    using namespace analyzer::ffi;
    if (config == nullptr) {
        set_status(status, status_failure("null configuration"));
        return nullptr;
    }

    return guard_status<AnalyzerSession*>(status, nullptr, [&]() -> AnalyzerSession* {
        const AnalyzerSessionConfig copy = *config;
        std::optional<std::string> uid;
        if (copy.device_uid != nullptr) {
            uid = checked_utf8(copy.device_uid);
            if (!uid) {
                set_status(status, status_failure("device uid is not valid UTF-8"));
                return nullptr;
            }
        }

        const std::unique_ptr<analyzer::audio::AudioBackend> backend =
            analyzer::audio::default_backend();
        std::unique_ptr<AnalyzerSession> session = start_session(copy, uid, *backend);
        set_status(status, status_ok());
        return session.release();
    });
}

extern "C" void analyzer_session_stop(AnalyzerSession* session) noexcept {
    if (session == nullptr) {
        return;
    }
    analyzer::ffi::guard([&] {
        const std::unique_ptr<AnalyzerSession> owned(session);
        owned->stop();
    });
}
