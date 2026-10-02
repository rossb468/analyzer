#include "cli/run.hpp"

#include <cerrno>
#include <exception>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "audio/error.hpp"
#include "audio/offline.hpp"
#include "cli/args.hpp"
#include "cli/bench.hpp"
#include "cli/error.hpp"
#include "cli/live.hpp"
#include "cli/measure.hpp"
#include "cli/report.hpp"
#include "cli/source.hpp"
#include "cli/text.hpp"
#include "dsp/spectrum.hpp"
#include "engine/engine.hpp"
#include "engine/ring.hpp"
#include "engine/rt.hpp"
#include "model/compare.hpp"
#include "model/wav.hpp"

namespace analyzer::cli {

namespace {

// The operating system's words for the last failed call, in the shape Rust's
// io::Error printed them: "No such file or directory (os error 2)".
std::string os_error() {
    const int code = errno;
    if (code == 0) {
        return "unknown error";
    }
    return std::error_code(code, std::generic_category()).message() + " (os error " +
           std::to_string(code) + ")";
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        throw CliError("opening " + path.string() + ": " + os_error());
    }
    // Through rdbuf() rather than an istreambuf_iterator pair: the iterator form
    // trips GCC's -Wnull-dereference in release builds, and is slower besides.
    // An empty file leaves failbit set on the insertion, which is not an error.
    std::ostringstream text;
    text << file.rdbuf();
    if (file.bad()) {
        throw CliError("opening " + path.string() + ": " + os_error());
    }
    return text.str();
}

model::Response read_response(const std::filesystem::path& path) {
    model::Response parsed = model::Response::parse(read_text(path));
    if (parsed.points.empty()) {
        throw CliError("no frequency and level pairs found in " + path.string());
    }
    return parsed;
}

std::string generate(const GenerateInput& input) {
    const std::size_t frames =
        model::write_signal(input.out, input.signal, static_cast<float>(input.rate),
                            static_cast<float>(input.seconds), input.depth);
    return "# wrote " + std::to_string(frames) + " frames (" +
           text::fixed(static_cast<double>(frames) / input.rate, 3) + " s) at " +
           text::display(input.rate) + " Hz, " + std::string(model::as_key(input.depth)) + " to " +
           input.out.string() + "\n";
}

std::string compare(const CompareInput& input, const Args& args) {
    const model::Response subject = read_response(input.subject);
    const model::Response reference = read_response(input.reference);
    const model::Comparison result = model::compare(subject, reference, args.from_hz, args.to_hz);

    // A tolerance turns this into a gate rather than a readout, so the parity
    // run can be a command that passes or fails.
    if (args.tolerance && !result.agrees_within(*args.tolerance)) {
        throw CliError(result.report() + "\nshapes differ by " +
                       text::fixed(result.max_deviation_after_offset, 4) + " dB at " +
                       text::fixed(result.max_deviation_after_offset_hz, 1) + " Hz, tolerance " +
                       text::fixed(*args.tolerance, 4) + " dB");
    }
    return result.report();
}

std::string render_live(const engine::SpectrumFrame& frame, const Args& args,
                        const LiveOptions& options) {
    // Rebuild the analyzer purely to recover the window's ENBW and hop for the
    // header; it never sees a sample.
    const dsp::SpectrumAnalyzer reference(args.spectrum(frame.sample_rate));
    return render(frame,
                  Meta{
                      .source = options.device.value_or("default input (live)"),
                      .channels = 1,
                      .channel = args.channel,
                      .sample_rate = static_cast<double>(frame.sample_rate),
                      .window = args.window,
                      .overlap = args.overlap,
                      .averaging = args.average,
                      .enbw_hz = reference.enbw_hz(),
                      .fft_size = reference.size(),
                      .hop = reference.hop(),
                  },
                  args.min_db, args.peak_only);
}

std::string analyse_offline(audio::Source source, const Args& args) {
    const double rate = source.sample_rate;
    const std::size_t channels = source.channels;
    const std::size_t frames = source.frames();

    auto ring = engine::capture_ring(channels, 8192);
    engine::CaptureSink& sink = ring.first;
    engine::CaptureSource& capture = ring.second;

    audio::OfflineBackend backend(std::move(source), args.block);
    audio::StreamConfig config;
    config.input = audio::DeviceId(audio::kOfflineDeviceId);
    config.sample_rate = rate;
    config.buffer_frames = static_cast<std::uint32_t>(args.block);
    for (std::size_t channel = 0; channel < channels; ++channel) {
        config.input_channels.push_back(static_cast<std::uint32_t>(channel));
    }

    std::unique_ptr<audio::AudioCallback> callback =
        audio::make_callback([&sink](audio::AudioBuffers& buffers) {
            engine::rt_section([&] { static_cast<void>(sink.write_interleaved(buffers.input())); });
        });

    audio::OfflineStream stream = [&] {
        try {
            return backend.open_offline(config, std::move(callback));
        } catch (const audio::AudioError& error) {
            throw CliError(std::string("opening offline stream: ") + error.what());
        }
    }();

    dsp::SpectrumAnalyzer analyzer(args.spectrum(static_cast<float>(rate)));
    std::vector<float> interleaved(args.block * channels, 0.0f);
    std::vector<float> channel_scratch(args.block, 0.0f);

    while (stream.pump() != 0) {
        while (capture.frames_available() > 0) {
            const std::size_t read = capture.read_interleaved(interleaved);
            if (read == 0) {
                break;
            }
            for (std::size_t frame = 0; frame < read; ++frame) {
                channel_scratch[frame] = interleaved[frame * channels + args.channel];
            }
            analyzer.push(std::span<const float>(channel_scratch).first(read));
        }
    }

    const std::uint64_t overruns = capture.overruns();
    if (overruns > 0) {
        throw CliError(std::to_string(overruns) + " block(s) dropped - the measurement is invalid");
    }
    if (analyzer.frames() == 0) {
        throw CliError("no complete frames were analysed");
    }

    engine::SpectrumFrame frame;
    frame.sequence = 1;
    frame.bins.assign(analyzer.bins(), 0.0f);
    analyzer.write_db_fs(frame.bins);
    frame.bin_spacing_hz = analyzer.bin_spacing_hz();
    frame.sample_rate = static_cast<float>(rate);
    frame.frames_averaged = analyzer.frames();
    frame.overruns = overruns;

    return render(frame,
                  Meta{
                      .source = std::to_string(frames) + " frames offline",
                      .channels = channels,
                      .channel = args.channel,
                      .sample_rate = rate,
                      .window = args.window,
                      .overlap = args.overlap,
                      .averaging = args.average,
                      .enbw_hz = analyzer.enbw_hz(),
                      .fft_size = analyzer.size(),
                      .hop = analyzer.hop(),
                  },
                  args.min_db, args.peak_only);
}

audio::Source load_source(const Input& input) {
    if (const auto* wav = std::get_if<WavInput>(&input)) {
        return read_source(wav->path);
    }
    if (const auto* sine = std::get_if<SineInput>(&input)) {
        return synthesise_sine(sine->hz, sine->rate, sine->seconds, sine->amplitude);
    }
    throw CliError("this mode does not load a source");
}

MeasureOptions measure_options(const Args& args) {
    MeasureOptions options;
    options.gate_ms = args.gate_ms;
    options.fft = args.fft;
    return options;
}

// Build the report for whichever mode was asked for.
std::string report_for(const Args& args) {
    struct Visitor {
        const Args& args;

        std::string operator()(const ListDevicesInput&) const { return list_devices(); }
        std::string operator()(const BenchInput& input) const { return run_bench(input.seconds); }
        std::string operator()(const MeasureDemoInput&) const {
            return demo(measure_options(args));
        }
        std::string operator()(const GenerateInput& input) const { return generate(input); }
        std::string operator()(const CompareInput& input) const { return compare(input, args); }
        std::string operator()(const MeasureInput& input) const {
            return from_files(input.stimulus, input.response, measure_options(args));
        }
        std::string operator()(const LiveInput& input) const {
            LiveOptions options;
            options.device = input.device;
            options.seconds = input.seconds;
            options.channel = args.channel;
            options.block = static_cast<std::uint32_t>(args.block);
            // Rate is replaced with whatever the device grants.
            options.spectrum = args.spectrum(48'000.0f);
            options.meter = args.meter;
            const engine::SpectrumFrame frame = capture(options);
            return render_live(frame, args, options);
        }
        std::string operator()(const WavInput&) const { return offline(); }
        std::string operator()(const SineInput&) const { return offline(); }

        std::string offline() const {
            audio::Source source = load_source(args.input);
            if (source.frames() < args.fft) {
                throw CliError("need at least " + std::to_string(args.fft) + " frames for a " +
                               std::to_string(args.fft) + "-point FFT, source has " +
                               std::to_string(source.frames()));
            }
            if (args.channel >= source.channels) {
                throw CliError("channel " + std::to_string(args.channel) +
                               " requested but the source has " + std::to_string(source.channels));
            }
            return analyse_offline(std::move(source), args);
        }
    };
    return std::visit(Visitor{args}, args.input);
}

void execute(std::span<const std::string> argv, std::ostream& out, std::ostream& err) {
    const std::optional<Args> args = parse_args(argv);
    if (!args) {
        out << usage();
        return;
    }

    const std::string report = report_for(*args);

    // --generate has already used --out for the audio itself, so its summary
    // goes to stdout. Writing the report there too would overwrite the WAV with
    // a line of text describing it.
    const bool report_to_file = args->out && !std::holds_alternative<GenerateInput>(args->input);

    if (report_to_file) {
        const std::filesystem::path& path = *args->out;
        std::ofstream file(path, std::ios::binary | std::ios::trunc);
        file.write(report.data(), static_cast<std::streamsize>(report.size()));
        file.close();
        if (!file) {
            throw CliError("writing " + path.string() + ": " + os_error());
        }
        err << "wrote " << path.string() << '\n';
    } else {
        out.write(report.data(), static_cast<std::streamsize>(report.size()));
        out.flush();
        if (!out) {
            throw CliError("writing stdout: " + os_error());
        }
    }
}

}  // namespace

int run(std::span<const std::string> argv, std::ostream& out, std::ostream& err) {
    try {
        execute(argv, out, err);
        return 0;
    } catch (const std::exception& error) {
        err << "analyzer-cli: " << error.what() << '\n';
        return 1;
    }
}

}  // namespace analyzer::cli
