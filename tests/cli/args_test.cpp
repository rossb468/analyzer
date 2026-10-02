#include "cli/args.hpp"

#include <gtest/gtest.h>

#include <initializer_list>
#include <string>
#include <vector>

#include "cli/error.hpp"

namespace analyzer::cli {
namespace {

std::optional<Args> parse(std::initializer_list<const char*> items) {
    const std::vector<std::string> argv(items.begin(), items.end());
    return parse_args(argv);
}

// The message parsing throws, or empty if it did not.
std::string error_of(std::initializer_list<const char*> items) {
    try {
        static_cast<void>(parse(items));
    } catch (const CliError& error) {
        return error.what();
    }
    return {};
}

template <class Mode>
const Mode& mode(const std::optional<Args>& args) {
    return std::get<Mode>(args.value().input);
}

TEST(Args, DefaultsMatchTheRustHarness) {
    const auto args = parse({"in.wav"});
    ASSERT_TRUE(args.has_value());
    EXPECT_EQ(mode<WavInput>(args).path, "in.wav");
    EXPECT_EQ(args->fft, 4096u);
    EXPECT_EQ(args->window, dsp::WindowKind::hann());
    EXPECT_EQ(args->overlap, dsp::Overlap::ThreeQuarters);
    EXPECT_EQ(args->average, dsp::Averaging::infinite());
    EXPECT_EQ(args->channel, 0u);
    EXPECT_EQ(args->block, 128u);
    EXPECT_FALSE(args->min_db.has_value());
    EXPECT_FALSE(args->peak_only);
    EXPECT_TRUE(args->meter);
    EXPECT_EQ(args->gate_ms, 5.0f);
    EXPECT_EQ(args->from_hz, 20.0);
    EXPECT_EQ(args->to_hz, 20'000.0);
    EXPECT_FALSE(args->tolerance.has_value());
}

TEST(Args, HelpReturnsNothingToRun) {
    EXPECT_FALSE(parse({"--help"}).has_value());
    EXPECT_FALSE(parse({"-h"}).has_value());
    // Help wins as soon as it is seen, even ahead of a later mistake.
    EXPECT_FALSE(parse({"--help", "--frobnicate"}).has_value());
}

TEST(Args, WindowNamesAndTheirAliases) {
    EXPECT_EQ(parse({"--sine", "1", "--window", "rect"})->window, dsp::WindowKind::rectangular());
    EXPECT_EQ(parse({"--sine", "1", "--window", "rectangular"})->window,
              dsp::WindowKind::rectangular());
    EXPECT_EQ(parse({"--sine", "1", "--window", "bh"})->window, dsp::WindowKind::blackman_harris());
    EXPECT_EQ(parse({"--sine", "1", "--window", "blackman-harris"})->window,
              dsp::WindowKind::blackman_harris());
    EXPECT_EQ(parse({"--sine", "1", "--window", "flattop"})->window, dsp::WindowKind::flat_top());
    EXPECT_EQ(parse({"--sine", "1", "--window", "flat-top"})->window, dsp::WindowKind::flat_top());
    EXPECT_EQ(parse({"--sine", "1", "--window", "tukey"})->window, dsp::WindowKind::tukey(0.25f));
}

TEST(Args, OverlapAndAveragingNames) {
    EXPECT_EQ(parse({"--sine", "1", "--overlap", "0"})->overlap, dsp::Overlap::None);
    EXPECT_EQ(parse({"--sine", "1", "--overlap", "50"})->overlap, dsp::Overlap::Half);
    EXPECT_EQ(parse({"--sine", "1", "--overlap", "87.5"})->overlap, dsp::Overlap::SevenEighths);
    EXPECT_EQ(parse({"--sine", "1", "--average", "none"})->average, dsp::Averaging::none());
    EXPECT_EQ(parse({"--sine", "1", "--average", "inf"})->average, dsp::Averaging::infinite());
    EXPECT_EQ(parse({"--sine", "1", "--average", "peak"})->average, dsp::Averaging::peak_hold());
}

// --live and --bench take an optional duration, so they must take the next
// token only when it is a number, not when it is another flag.
TEST(Args, OptionalDurationsAreTakenOnlyWhenTheyLookLikeNumbers) {
    EXPECT_EQ(mode<BenchInput>(parse({"--bench"})).seconds, 2.0);
    EXPECT_EQ(mode<BenchInput>(parse({"--bench", "0.2"})).seconds, 0.2);
    EXPECT_EQ(mode<BenchInput>(parse({"--bench", "--fft", "1024"})).seconds, 2.0);
    EXPECT_EQ(mode<LiveInput>(parse({"--live"})).seconds, 5.0);
    EXPECT_EQ(mode<LiveInput>(parse({"--live", "3", "--device", "abc"})).seconds, 3.0);
    EXPECT_EQ(mode<LiveInput>(parse({"--live", "--device", "abc"})).device, "abc");
    // A path is not a number, so it is a second input rather than a duration.
    EXPECT_NE(error_of({"--bench", "file.wav"}).find("choose one of"), std::string::npos);
}

TEST(Args, SineAndSweepDefaultToDifferentFrequencies) {
    const auto sine = parse({"--generate", "sine", "--out", "a.wav"});
    EXPECT_EQ(mode<GenerateInput>(sine).signal, dsp::Signal::sine(1000.0f, 0.5f));

    const auto sweep = parse({"--generate", "sweep", "--out", "a.wav", "--seconds", "2"});
    EXPECT_EQ(mode<GenerateInput>(sweep).signal,
              dsp::Signal::sweep(20.0f, 20'000.0f, 2.0f, 0.5f, false));

    const auto given = parse({"--generate", "sweep", "--hz", "100", "--hz-end", "5000", "--out",
                              "a.wav", "--depth", "i16"});
    EXPECT_EQ(mode<GenerateInput>(given).signal,
              dsp::Signal::sweep(100.0f, 5000.0f, 1.0f, 0.5f, false));
    EXPECT_EQ(mode<GenerateInput>(given).depth, model::SampleDepth::Int16);
}

TEST(Args, GenerateClampsTheAmplitude) {
    const auto loud = parse({"--generate", "white", "--amplitude", "7", "--out", "a.wav"});
    EXPECT_EQ(mode<GenerateInput>(loud).signal, dsp::Signal::white_noise(1.0f));
}

TEST(Args, ErrorsCarryTheRustMessages) {
    EXPECT_EQ(error_of({}), "no input given (try --help)");
    EXPECT_EQ(error_of({"--frobnicate"}), "unknown option '--frobnicate' (try --help)");
    EXPECT_EQ(error_of({"a.wav", "b.wav"}), "more than one input path given");
    EXPECT_EQ(error_of({"--gate"}), "--gate needs a value (try --help)");
    EXPECT_EQ(error_of({"--measure", "only-one.wav"}), "--measure needs a value (try --help)");
    EXPECT_EQ(error_of({"--fft", "abc"}), "--fft: cannot parse 'abc'");
    EXPECT_EQ(error_of({"--fft", "-4"}), "--fft: cannot parse '-4'");
    EXPECT_EQ(error_of({"--sine", "1", "--fft", "1001"}),
              "--fft must be even and at least 2, got 1001");
    EXPECT_EQ(error_of({"--sine", "1", "--fft", "0"}), "--fft must be even and at least 2, got 0");
    EXPECT_EQ(error_of({"--sine", "1", "--block", "0"}), "--block must be non-zero");
    EXPECT_EQ(error_of({"--sine", "1", "--window", "nope"}), "unknown window 'nope'");
    EXPECT_EQ(error_of({"--sine", "1", "--overlap", "60"}),
              "unknown overlap '60', want 0/50/75/87");
    EXPECT_EQ(error_of({"--sine", "1", "--average", "x"}), "unknown averaging 'x'");
    EXPECT_EQ(error_of({"--generate", "sine", "--depth", "i8", "--out", "a.wav"}),
              "--depth: unknown depth 'i8' (i16, i24, f32)");
    EXPECT_EQ(error_of({"--generate", "sine"}), "--generate needs --out <file.wav>");
    EXPECT_EQ(error_of({"--generate", "triangle", "--out", "a.wav"}),
              "--generate: unknown signal 'triangle' (sine, pink, white, sweep)");
    EXPECT_EQ(error_of({"--generate", "sine", "--out", "a.wav", "--seconds", "0"}),
              "--seconds must be positive");
    EXPECT_EQ(error_of({"--sine", "1", "--rate", "0"}), "--rate must be positive");
    EXPECT_EQ(error_of({"--bench", "0"}), "--bench duration must be positive");
    EXPECT_EQ(error_of({"--live", "-1"}), "--live duration must be positive");
    EXPECT_EQ(error_of({"--compare", "a", "b", "--from-hz", "500", "--to-hz", "100"}),
              "--from-hz must be positive and below --to-hz");
    EXPECT_EQ(error_of({"--compare", "a", "b", "--to-hz", "inf"}),
              "--from-hz must be positive and below --to-hz");
}

TEST(Args, TheInputModesAreMutuallyExclusive) {
    for (const auto& items :
         std::vector<std::vector<const char*>>{{"--sine", "1000", "some.wav"},
                                               {"--live", "1", "--sine", "1000"},
                                               {"--list-devices", "--sine", "1000"},
                                               {"--measure-demo", "--bench"},
                                               {"--compare", "a", "b", "--generate", "sine"}}) {
        const std::vector<std::string> argv(items.begin(), items.end());
        try {
            static_cast<void>(parse_args(argv));
            ADD_FAILURE() << items.front() << " ... should have been refused";
        } catch (const CliError& error) {
            EXPECT_EQ(std::string(error.what()),
                      "choose one of: a WAV path, --sine, --live, --list-devices, --bench, "
                      "--generate, --compare");
        }
    }
}

TEST(Args, SpectrumConfigCarriesTheAnalysisFlags) {
    const auto args = parse(
        {"--sine", "1", "--fft", "2048", "--window", "bh", "--overlap", "50", "--average", "peak"});
    const dsp::SpectrumConfig config = args->spectrum(44'100.0f);
    EXPECT_EQ(config.sample_rate, 44'100.0f);
    EXPECT_EQ(config.size, 2048u);
    EXPECT_EQ(config.window, dsp::WindowKind::blackman_harris());
    EXPECT_EQ(config.overlap, dsp::Overlap::Half);
    EXPECT_EQ(config.averaging, dsp::Averaging::peak_hold());
}

TEST(Args, UsageDescribesEveryInputMode) {
    const std::string_view text = usage();
    for (const char* flag : {"--sine", "--live", "--list-devices", "--bench", "--measure",
                             "--measure-demo", "--generate", "--compare", "--window"}) {
        EXPECT_NE(text.find(flag), std::string_view::npos) << flag;
    }
}

}  // namespace
}  // namespace analyzer::cli
