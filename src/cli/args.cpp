#include "cli/args.hpp"

#include <algorithm>
#include <cmath>
#include <type_traits>
#include <utility>

#include "base/number_text.hpp"
#include "cli/error.hpp"

namespace analyzer::cli {

namespace {

// Raw so that the text reads as it prints. The golden help fixture is this,
// byte for byte.
constexpr std::string_view kUsage = R"usage(analyzer-cli - headless spectrum analysis harness

USAGE:
    analyzer-cli [OPTIONS] <input.wav>
    analyzer-cli [OPTIONS] --sine <hz>
    analyzer-cli [OPTIONS] --live [seconds]
    analyzer-cli --list-devices

INPUT:
    <input.wav>          WAV file (16/24/32-bit integer or 32-bit float)
    --sine <hz>          Synthesise a sine instead of reading a file
    --live [seconds]     Capture from hardware (default 5 seconds)
    --list-devices       Show every audio device and exit
    --bench [seconds]    Measure analysis throughput and ring behaviour (default 2)

SWEPT MEASUREMENT:
    --measure <a> <b>    Deconvolve response <b> against stimulus <a>
    --measure-demo       Build a synthetic room and measure it end to end
    --gate <ms>          Gate length for the quasi-anechoic response (default 5)

SIGNAL GENERATION:
    --generate <kind>    Write a test signal: sine | pink | white | sweep
    --out <file.wav>     Where to write it (required with --generate)
    --depth <d>          i16 | i24 | f32 (default f32)
    --hz <n>             Sine frequency, or sweep start (default 1000 / 20)
    --hz-end <n>         Sweep end frequency (default 20000)

COMPARISON:
    --compare <a> <b>    Compare two frequency/level exports, <a> against <b>
    --from-hz <n>        Low end of the compared band (default 20)
    --to-hz <n>          High end of the compared band (default 20000)
    --tolerance <db>     Fail unless the shapes agree this closely

ANALYSIS:
    --fft <n>            FFT size, even (default 4096)
    --window <name>      rect | hann | bh | flattop | tukey (default hann)
    --overlap <pct>      0 | 50 | 75 | 87 (default 75)
    --average <mode>     none | infinite | peak (default infinite)
    --channel <n>        Which input channel to analyse (default 0)
    --block <frames>     Callback block size (default 128)

LIVE:
    --device <uid>       Capture device UID (default: system default input)
    --no-meter           Suppress the running level meter

SYNTHESIS:
    --rate <hz>          Sample rate for --sine (default 48000)
    --seconds <s>        Duration for --sine (default 1.0)
    --amplitude <a>      Peak amplitude for --sine (default 0.5)

OUTPUT:
    --min-db <db>        Omit bins quieter than this
    --peak               Print only the loudest bin
    --out <path>         Write to a file instead of stdout
    -h, --help           This text

Levels are dBFS with 0 dBFS = full-scale sine. Phase is not emitted: a
single-channel spectrum has no phase reference, and padding the column with
zeros would be fabricating data.
)usage";

[[noreturn]] void fail(const std::string& message) {
    throw CliError(message);
}

// A number flag's value, with Rust's `str::parse` rules and the harness's
// message when it does not read.
template <class T>
T number(const std::string& raw, const std::string& flag) {
    std::optional<T> parsed;
    if constexpr (std::is_same_v<T, double>) {
        parsed = text::parse_f64(raw);
    } else if constexpr (std::is_same_v<T, float>) {
        parsed = text::parse_f32(raw);
    } else {
        parsed = text::parse_usize(raw);
    }
    if (!parsed) {
        fail(flag + ": cannot parse '" + raw + "'");
    }
    return *parsed;
}

// Walks the argument list the way the Rust popped it: a flag asks for its
// value, and an optional value is taken only when the next token is a number.
class Cursor {
public:
    explicit Cursor(std::span<const std::string> argv) : argv_(argv) {}

    bool done() const noexcept { return next_ >= argv_.size(); }

    const std::string& take() { return argv_[next_++]; }

    // The value for `flag`, or the error that says it was missing.
    const std::string& value(const std::string& flag) {
        if (done()) {
            fail(flag + " needs a value (try --help)");
        }
        return take();
    }

    // For `--live [seconds]` and `--bench [seconds]`: consume the next token
    // only when it is a number rather than another flag or a path.
    double optional_seconds(double fallback) {
        if (!done()) {
            if (const auto seconds = text::parse_f64(argv_[next_])) {
                ++next_;
                return *seconds;
            }
        }
        return fallback;
    }

private:
    std::span<const std::string> argv_;
    std::size_t next_ = 0;
};

dsp::WindowKind parse_window(const std::string& name) {
    if (name == "rect" || name == "rectangular") {
        return dsp::WindowKind::rectangular();
    }
    if (name == "hann") {
        return dsp::WindowKind::hann();
    }
    if (name == "bh" || name == "blackman-harris") {
        return dsp::WindowKind::blackman_harris();
    }
    if (name == "flattop" || name == "flat-top") {
        return dsp::WindowKind::flat_top();
    }
    if (name == "tukey") {
        return dsp::WindowKind::tukey(0.25f);
    }
    fail("unknown window '" + name + "'");
}

dsp::Overlap parse_overlap(const std::string& percent) {
    if (percent == "0") {
        return dsp::Overlap::None;
    }
    if (percent == "50") {
        return dsp::Overlap::Half;
    }
    if (percent == "75") {
        return dsp::Overlap::ThreeQuarters;
    }
    if (percent == "87" || percent == "87.5") {
        return dsp::Overlap::SevenEighths;
    }
    fail("unknown overlap '" + percent + "', want 0/50/75/87");
}

dsp::Averaging parse_averaging(const std::string& mode) {
    if (mode == "none") {
        return dsp::Averaging::none();
    }
    if (mode == "infinite" || mode == "inf") {
        return dsp::Averaging::infinite();
    }
    if (mode == "peak") {
        return dsp::Averaging::peak_hold();
    }
    fail("unknown averaging '" + mode + "'");
}

}  // namespace

std::string_view usage() noexcept {
    return kUsage;
}

dsp::SpectrumConfig Args::spectrum(float sample_rate) const {
    return dsp::SpectrumConfig{
        .sample_rate = sample_rate,
        .size = fft,
        .window = window,
        .overlap = overlap,
        .averaging = average,
    };
}

std::optional<Args> parse_args(std::span<const std::string> argv) {
    Args args;

    std::optional<std::filesystem::path> positional;
    std::optional<double> sine_hz;
    std::optional<double> live_seconds;
    bool list_devices = false;
    std::optional<double> bench_seconds;
    std::optional<std::string> device;
    double rate = 48'000.0;
    double seconds = 1.0;
    float amplitude = 0.5f;
    bool measure_demo = false;
    std::optional<std::pair<std::filesystem::path, std::filesystem::path>> measure_pair;
    std::optional<std::string> generate;
    std::optional<std::pair<std::filesystem::path, std::filesystem::path>> compare_pair;
    model::SampleDepth depth = model::kDefaultSampleDepth;
    double hz = 1000.0;
    double hz_end = 20'000.0;
    bool hz_given = false;

    Cursor cursor(argv);
    while (!cursor.done()) {
        const std::string& arg = cursor.take();

        if (arg == "-h" || arg == "--help") {
            return std::nullopt;
        }
        if (arg == "--peak") {
            args.peak_only = true;
        } else if (arg == "--no-meter") {
            args.meter = false;
        } else if (arg == "--list-devices") {
            list_devices = true;
        } else if (arg == "--measure-demo") {
            measure_demo = true;
        } else if (arg == "--gate") {
            args.gate_ms = number<float>(cursor.value(arg), arg);
        } else if (arg == "--measure") {
            std::filesystem::path stimulus = cursor.value(arg);
            std::filesystem::path response = cursor.value(arg);
            measure_pair = {std::move(stimulus), std::move(response)};
        } else if (arg == "--generate") {
            generate = cursor.value(arg);
        } else if (arg == "--compare") {
            std::filesystem::path subject = cursor.value(arg);
            std::filesystem::path reference = cursor.value(arg);
            compare_pair = {std::move(subject), std::move(reference)};
        } else if (arg == "--depth") {
            const std::string& raw = cursor.value(arg);
            const auto parsed = model::depth_from_key(raw);
            if (!parsed) {
                fail("--depth: unknown depth '" + raw + "' (i16, i24, f32)");
            }
            depth = *parsed;
        } else if (arg == "--hz") {
            hz = number<double>(cursor.value(arg), arg);
            hz_given = true;
        } else if (arg == "--hz-end") {
            hz_end = number<double>(cursor.value(arg), arg);
        } else if (arg == "--from-hz") {
            args.from_hz = number<double>(cursor.value(arg), arg);
        } else if (arg == "--to-hz") {
            args.to_hz = number<double>(cursor.value(arg), arg);
        } else if (arg == "--tolerance") {
            args.tolerance = number<double>(cursor.value(arg), arg);
        } else if (arg == "--bench") {
            bench_seconds = cursor.optional_seconds(2.0);
        } else if (arg == "--live") {
            live_seconds = cursor.optional_seconds(5.0);
        } else if (arg == "--device") {
            device = cursor.value(arg);
        } else if (arg == "--sine") {
            sine_hz = number<double>(cursor.value(arg), arg);
        } else if (arg == "--fft") {
            args.fft = number<std::size_t>(cursor.value(arg), arg);
        } else if (arg == "--channel") {
            args.channel = number<std::size_t>(cursor.value(arg), arg);
        } else if (arg == "--block") {
            args.block = number<std::size_t>(cursor.value(arg), arg);
        } else if (arg == "--rate") {
            rate = number<double>(cursor.value(arg), arg);
        } else if (arg == "--seconds") {
            seconds = number<double>(cursor.value(arg), arg);
        } else if (arg == "--amplitude") {
            amplitude = number<float>(cursor.value(arg), arg);
        } else if (arg == "--min-db") {
            args.min_db = number<float>(cursor.value(arg), arg);
        } else if (arg == "--out") {
            args.out = std::filesystem::path(cursor.value(arg));
        } else if (arg == "--window") {
            args.window = parse_window(cursor.value(arg));
        } else if (arg == "--overlap") {
            args.overlap = parse_overlap(cursor.value(arg));
        } else if (arg == "--average") {
            args.average = parse_averaging(cursor.value(arg));
        } else if (arg.starts_with('-')) {
            fail("unknown option '" + arg + "' (try --help)");
        } else {
            if (positional) {
                fail("more than one input path given");
            }
            positional = std::filesystem::path(arg);
        }
    }

    if (args.fft < 2 || args.fft % 2 != 0) {
        fail("--fft must be even and at least 2, got " + std::to_string(args.fft));
    }
    if (args.block == 0) {
        fail("--block must be non-zero");
    }

    const bool chosen[] = {positional.has_value(),    sine_hz.has_value(),
                           live_seconds.has_value(),  list_devices,
                           bench_seconds.has_value(), measure_demo,
                           measure_pair.has_value(),  generate.has_value(),
                           compare_pair.has_value()};
    const auto selected = std::ranges::count(chosen, true);
    if (selected > 1) {
        fail(
            "choose one of: a WAV path, --sine, --live, --list-devices, --bench, "
            "--generate, --compare");
    }

    if (compare_pair) {
        if (args.from_hz <= 0.0 || !std::isfinite(args.from_hz) || args.to_hz <= args.from_hz ||
            !std::isfinite(args.to_hz)) {
            fail("--from-hz must be positive and below --to-hz");
        }
        args.input = CompareInput{std::move(compare_pair->first), std::move(compare_pair->second)};
    } else if (generate) {
        if (!args.out) {
            fail("--generate needs --out <file.wav>");
        }
        if (rate <= 0.0) {
            fail("--rate must be positive");
        }
        if (seconds <= 0.0) {
            fail("--seconds must be positive");
        }
        const float level = std::clamp(amplitude, 0.0f, 1.0f);
        dsp::Signal signal;
        if (*generate == "sine") {
            // The sine default is 1 kHz; the sweep's is 20 Hz. Sharing one flag
            // means the default has to depend on which was asked for.
            signal = dsp::Signal::sine(hz_given ? static_cast<float>(hz) : 1000.0f, level);
        } else if (*generate == "pink") {
            signal = dsp::Signal::pink_noise(level);
        } else if (*generate == "white") {
            signal = dsp::Signal::white_noise(level);
        } else if (*generate == "sweep") {
            // One pass filling the file exactly. A repeating sweep would
            // overlap its own tail and deconvolve into a second arrival.
            signal = dsp::Signal::sweep(hz_given ? static_cast<float>(hz) : 20.0f,
                                        static_cast<float>(hz_end), static_cast<float>(seconds),
                                        level, false);
        } else {
            fail("--generate: unknown signal '" + *generate + "' (sine, pink, white, sweep)");
        }
        args.input = GenerateInput{signal, rate, seconds, depth, *args.out};
    } else if (list_devices) {
        args.input = ListDevicesInput{};
    } else if (measure_demo) {
        args.input = MeasureDemoInput{};
    } else if (measure_pair) {
        args.input = MeasureInput{std::move(measure_pair->first), std::move(measure_pair->second)};
    } else if (bench_seconds) {
        if (*bench_seconds <= 0.0) {
            fail("--bench duration must be positive");
        }
        args.input = BenchInput{*bench_seconds};
    } else if (live_seconds) {
        if (*live_seconds <= 0.0) {
            fail("--live duration must be positive");
        }
        args.input = LiveInput{std::move(device), *live_seconds};
    } else if (positional) {
        args.input = WavInput{std::move(*positional)};
    } else if (sine_hz) {
        if (rate <= 0.0) {
            fail("--rate must be positive");
        }
        args.input = SineInput{*sine_hz, rate, seconds, amplitude};
    } else {
        fail("no input given (try --help)");
    }

    return args;
}

}  // namespace analyzer::cli
