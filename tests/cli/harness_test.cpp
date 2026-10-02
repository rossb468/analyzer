// End-to-end tests for the harness.
//
// These drive the built binary as a subprocess rather than calling into a
// library, so they exercise the real thing: argument parsing, the audio backend,
// the allocation trap around the callback, the capture ring, and the analyzer.
// If the callback ever allocates, the child aborts and every test here fails -
// which is the point. (The trap is linked into debug builds only, as the Rust
// harness's global allocator was; see src/cli/CMakeLists.txt.)
//
// Ports tools/analyzer-cli/tests/harness.rs.

#if !defined(_WIN32)

#include <gtest/gtest.h>
#include <unistd.h>

#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <sstream>
#include <string>
#include <vector>

#include "audio/target.hpp"
#include "model/wav.hpp"
#include "subprocess.hpp"

namespace analyzer::cli {
namespace {

using harness::Output;

const std::string kBin = ANALYZER_CLI_PATH;

// Level of a full-scale-referenced sine at amplitude 0.5: 20*log10(0.5).
constexpr float kHalfScaleDb = -6.0206f;

// A frequency that lands exactly on bin 85 at 48 kHz with a 4096-point FFT.
// On-bin means no scalloping loss, so every window must agree.
const std::string kOnBinHz = "996.09375";

Output run(const std::vector<std::string>& args) {
    return harness::run_process(kBin, args);
}

bool contains(const std::string& text, const std::string& needle) {
    return text.find(needle) != std::string::npos;
}

// A path in the temp directory that no other test, and no other run of the same
// test, will use: ctest runs the tests of this binary in parallel processes.
std::filesystem::path temp_path(const std::string& name) {
    return std::filesystem::temp_directory_path() /
           ("analyzer-harness-" + std::to_string(getpid()) + "-" + name);
}

std::vector<std::string> lines(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

bool is_blank(const std::string& line) {
    return line.find_first_not_of(" \t\r\n") == std::string::npos;
}

std::size_t count_rows(const std::string& stdout_text) {
    std::size_t rows = 0;
    for (const std::string& line : lines(stdout_text)) {
        if (!line.starts_with('#') && !is_blank(line)) {
            ++rows;
        }
    }
    return rows;
}

struct PeakRow {
    float frequency;
    float level;
};

// Parse the single `frequency\tlevel` row produced by `--peak`.
PeakRow peak(const std::vector<std::string>& args) {
    const Output output = run(args);
    EXPECT_TRUE(output.success()) << "harness failed: " << output.err;
    for (const std::string& line : lines(output.out)) {
        if (line.starts_with('#') || is_blank(line)) {
            continue;
        }
        const std::size_t tab = line.find('\t');
        EXPECT_NE(tab, std::string::npos) << "bad row " << line;
        return {std::stof(line.substr(0, tab)), std::stof(line.substr(tab + 1))};
    }
    ADD_FAILURE() << "no data row in output:\n" << output.out;
    return {0.0f, 0.0f};
}

std::vector<std::string> concat(std::vector<std::string> first,
                                const std::vector<std::string>& second) {
    first.insert(first.end(), second.begin(), second.end());
    return first;
}

// The load-bearing test. An on-bin sine at amplitude 0.5 must read -6.02 dBFS
// through every window, which only holds if the FFT scaling, the window
// correction factors and the dBFS reference are all correct together.
TEST(Harness, OnBinSineReadsCorrectLevelThroughEveryWindow) {
    for (const std::string window : {"rect", "hann", "bh", "flattop", "tukey"}) {
        const PeakRow row = peak({"--sine", kOnBinHz, "--amplitude", "0.5", "--seconds", "1",
                                  "--window", window, "--peak"});
        EXPECT_LT(std::abs(row.frequency - 996.09375f), 0.001f)
            << window << ": peak at " << row.frequency << " Hz";
        EXPECT_LT(std::abs(row.level - kHalfScaleDb), 0.02f)
            << window << ": read " << row.level << " dBFS, expected " << kHalfScaleDb;
    }
}

// Off-bin tones lose level to scalloping, and how much is the whole reason
// several windows exist. Flat-top is nearly immune, which is why it is the
// calibration window; Hann is not.
TEST(Harness, FlatTopResistsScallopingLossFarBetterThanHann) {
    const std::vector<std::string> off_bin = {"--sine", "1000",      "--amplitude",
                                              "0.5",    "--seconds", "1"};

    const float hann_db = peak(concat(off_bin, {"--window", "hann", "--peak"})).level;
    const float flat_db = peak(concat(off_bin, {"--window", "flattop", "--peak"})).level;

    const float hann_loss = std::abs(kHalfScaleDb - hann_db);
    const float flat_loss = std::abs(kHalfScaleDb - flat_db);

    // Flat-top's published scalloping loss is ~0.01 dB; Hann's worst case is
    // 1.42 dB. Assert the ordering and that flat-top is genuinely accurate.
    EXPECT_LT(flat_loss, 0.05f) << "flat-top should be near-exact off-bin, lost " << flat_loss
                                << " dB";
    EXPECT_GT(hann_loss, flat_loss * 4.0f)
        << "hann should lose clearly more than flat-top: " << hann_loss << " vs " << flat_loss;
    EXPECT_LT(hann_loss, 1.5f) << "hann loss should stay within its 1.42 dB worst case, got "
                               << hann_loss;
}

TEST(Harness, AmplitudeChangesLevelByTheExpectedDecibels) {
    const auto level_at = [](const std::string& amplitude) {
        return peak({"--sine", kOnBinHz, "--amplitude", amplitude, "--seconds", "1", "--window",
                     "flattop", "--peak"})
            .level;
    };

    const float full = level_at("1.0");
    const float half = level_at("0.5");
    const float quarter = level_at("0.25");

    EXPECT_LT(std::abs(full), 0.02f) << "full scale should be 0 dBFS, got " << full;
    // Halving amplitude is -6.02 dB, every time.
    EXPECT_LT(std::abs(full - half - 6.0206f), 0.03f) << full << " -> " << half;
    EXPECT_LT(std::abs(half - quarter - 6.0206f), 0.03f) << half << " -> " << quarter;
}

// The block size is the audio driver's business, not the analyzer's, so it must
// not change the answer.
TEST(Harness, CallbackBlockSizeDoesNotAffectTheResult) {
    const auto level_at = [](const std::string& block) {
        return peak({"--sine", kOnBinHz, "--amplitude", "0.5", "--seconds", "1", "--window", "hann",
                     "--block", block, "--peak"})
            .level;
    };

    const float reference = level_at("128");
    for (const std::string block : {"1", "64", "512", "4096", "5000"}) {
        const float level = level_at(block);
        EXPECT_LT(std::abs(level - reference), 1e-3f)
            << "block " << block << " gave " << level << ", reference " << reference;
    }
}

TEST(Harness, OverlapChangesFrameCountButNotLevel) {
    std::vector<float> levels;
    for (const std::string overlap : {"0", "50", "75", "87"}) {
        levels.push_back(peak({"--sine", kOnBinHz, "--amplitude", "0.5", "--seconds", "1",
                               "--window", "hann", "--overlap", overlap, "--peak"})
                             .level);
    }
    for (const float level : levels) {
        EXPECT_LT(std::abs(level - kHalfScaleDb), 0.02f) << "overlap changed the level";
    }
}

TEST(Harness, FullOutputCoversTheWholeSpectrumWithMetadata) {
    const Output output = run({"--sine", "1000", "--seconds", "1", "--fft", "1024"});
    EXPECT_TRUE(output.success());

    EXPECT_TRUE(contains(output.out, "# sample rate: 48000 Hz"));
    EXPECT_TRUE(contains(output.out, "# fft size: 1024"));
    EXPECT_TRUE(contains(output.out, "# level reference: 0 dBFS = full-scale sine"));

    // One row per bin: size / 2 + 1.
    EXPECT_EQ(count_rows(output.out), 513u) << "expected one row per bin";
}

TEST(Harness, MinDbFiltersQuietBins) {
    const Output all = run({"--sine", kOnBinHz, "--seconds", "1", "--fft", "1024"});
    const Output filtered =
        run({"--sine", kOnBinHz, "--seconds", "1", "--fft", "1024", "--min-db", "-60"});

    EXPECT_LT(count_rows(filtered.out), count_rows(all.out))
        << "--min-db should drop rows: " << count_rows(filtered.out) << " vs "
        << count_rows(all.out);
    EXPECT_GT(count_rows(filtered.out), 0u) << "should not drop the peak";
}

TEST(Harness, HelpSucceedsAndDescribesUsage) {
    const Output output = run({"--help"});
    EXPECT_TRUE(output.success());
    EXPECT_TRUE(contains(output.out, "USAGE:"));
    EXPECT_TRUE(contains(output.out, "--window"));
}

TEST(Harness, NoInputIsAnError) {
    const Output output = run({});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "no input"));
}

TEST(Harness, UnknownOptionIsAnError) {
    const Output output = run({"--nonsense"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "unknown option"));
}

TEST(Harness, OddFftSizeIsRejected) {
    const Output output = run({"--sine", "1000", "--fft", "1023"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "must be even"));
}

TEST(Harness, SourceShorterThanTheFftIsRejected) {
    // 100 frames of material against a 4096-point FFT.
    const Output output = run({"--sine", "1000", "--seconds", "0.002", "--fft", "4096"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "need at least")) << "stderr: " << output.err;
}

// The input modes are mutually exclusive; picking two must be refused rather
// than one silently winning.
TEST(Harness, CombiningInputModesIsAnError) {
    for (const auto& args : std::vector<std::vector<std::string>>{
             {"--sine", "1000", "some.wav"},
             {"--live", "1", "--sine", "1000"},
             {"--list-devices", "--sine", "1000"},
         }) {
        const Output output = run(args);
        EXPECT_FALSE(output.success()) << args.front() << " ... should have failed";
        EXPECT_TRUE(contains(output.err, "choose one of"))
            << args.front() << " ... gave: " << output.err;
    }
}

// ------------------------------------------------------------- generation --

// The parity run's first step. It must produce real audio, not a header.
TEST(Harness, GenerateWritesAPlayableFile) {
    const std::filesystem::path path = temp_path("generate.wav");
    std::filesystem::remove(path);

    const Output output =
        run({"--generate", "sine", "--hz", "1000", "--seconds", "1", "--out", path.string()});
    ASSERT_TRUE(output.success()) << output.err;

    const model::WavFile wav = model::read_wav(path);
    EXPECT_EQ(wav.sample_rate, 48'000.0);
    EXPECT_EQ(wav.channels, 1u);
    EXPECT_EQ(wav.samples.size(), 48'000u) << "one second at 48 kHz";
    bool audible = false;
    for (const float sample : wav.samples) {
        audible = audible || std::abs(sample) > 0.4f;
    }
    EXPECT_TRUE(audible) << "the file is silent - a header was written and the audio was not";

    std::filesystem::remove(path);
}

// --out is also the report destination, so generating had better not
// overwrite the audio with a line of text describing it.
TEST(Harness, GenerateDoesNotOverwriteItsOwnOutputWithTheReport) {
    const std::filesystem::path path = temp_path("clobber.wav");
    std::filesystem::remove(path);

    const Output output = run({"--generate", "pink", "--seconds", "1", "--out", path.string()});
    EXPECT_TRUE(output.success());
    const auto written = std::filesystem::file_size(path);
    EXPECT_GT(written, 100'000u) << "only " << written << " bytes - the report clobbered it";
    EXPECT_TRUE(contains(output.out, "wrote")) << "the summary should go to stdout";

    std::filesystem::remove(path);
}

TEST(Harness, GenerateRejectsAnUnknownSignalAndAMissingDestination) {
    Output output = run({"--generate", "trombone", "--out", "/tmp/x.wav"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "unknown signal"));

    output = run({"--generate", "sine"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "--out"));
}

// ------------------------------------------------------------- comparison --

std::filesystem::path write_temp(const std::string& name, const std::string& body) {
    const std::filesystem::path path = temp_path(name);
    std::ofstream(path, std::ios::binary) << body;
    return path;
}

// A pure reference-convention difference must not read as a parity failure,
// which is the whole reason the offset is reported separately.
TEST(Harness, CompareSeparatesAConstantOffsetFromTheShape) {
    const auto ours = write_temp("cmp-a.txt", "100\t0.0\n1000\t0.0\n10000\t0.0\n");
    const auto theirs = write_temp("cmp-b.txt", "100\t-3.01\n1000\t-3.01\n10000\t-3.01\n");

    const Output output = run({"--compare", ours.string(), theirs.string(), "--tolerance", "0.01"});
    EXPECT_TRUE(output.success()) << "a constant offset should pass: " << output.err;
    EXPECT_TRUE(contains(output.out, "+3.0100 dB")) << output.out;

    std::filesystem::remove(ours);
    std::filesystem::remove(theirs);
}

// And a shape difference must fail, with a non-zero exit so the parity run can
// be a script rather than something a human reads.
TEST(Harness, CompareFailsOnAFrequencyDependentDifference) {
    const auto ours = write_temp("cmp-c.txt", "100\t0.0\n1000\t0.0\n10000\t0.0\n");
    const auto theirs = write_temp("cmp-d.txt", "100\t-1.0\n1000\t0.0\n10000\t1.0\n");

    const Output output = run({"--compare", ours.string(), theirs.string(), "--tolerance", "0.1"});
    EXPECT_FALSE(output.success()) << "a 1 dB tilt must fail";
    EXPECT_TRUE(contains(output.err, "shapes differ"));

    std::filesystem::remove(ours);
    std::filesystem::remove(theirs);
}

TEST(Harness, CompareReportsAFileItCannotRead) {
    const Output output = run({"--compare", "nowhere.txt", "also-nowhere.txt"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "opening")) << "stderr: " << output.err;
    EXPECT_FALSE(contains(output.err, "panicked"));
}

// Generated, analysed, and compared against itself: the whole loop the parity
// run walks, minus REW.
TEST(Harness, AGeneratedSignalRoundTripsThroughAnalysisAndComparesToItself) {
    const std::filesystem::path wav = temp_path("roundtrip.wav");
    std::filesystem::remove(wav);

    ASSERT_TRUE(run({"--generate", "pink", "--seconds", "2", "--out", wav.string()}).success());

    const auto analyse = [&](const std::string& name) {
        const Output output = run({wav.string(), "--fft", "4096"});
        EXPECT_TRUE(output.success()) << output.err;
        return write_temp(name, output.out);
    };

    const auto first = analyse("rt-1.txt");
    const auto second = analyse("rt-2.txt");
    const Output output =
        run({"--compare", first.string(), second.string(), "--tolerance", "0.0001"});
    EXPECT_TRUE(output.success()) << "the same input analysed twice must agree exactly: "
                                  << output.err;

    std::filesystem::remove(wav);
    std::filesystem::remove(first);
    std::filesystem::remove(second);
}

#if ANALYZER_AUDIO_COREAUDIO
// Enumerating devices must not need capture permission. macOS prompts for the
// microphone on the first *capture*, and a device list that tripped that
// prompt would make the harness unusable for the one thing it is best at:
// finding out what the machine can see before anything is recorded.
TEST(Harness, ListDevicesSucceedsWithoutCapturePermission) {
    const Output output = run({"--list-devices"});
    EXPECT_TRUE(output.success());
    EXPECT_TRUE(contains(output.out, "Audio devices"));
}
#else
// Where there is no backend, the flag still parses and fails with an
// explanation rather than vanishing from the interface or aborting.
TEST(Harness, ListDevicesExplainsThatThereIsNoBackend) {
    const Output output = run({"--list-devices"});
    EXPECT_FALSE(output.success()) << "should fail, not pretend";
    EXPECT_TRUE(contains(output.err, "platform audio backend"))
        << "should say what is missing, got: " << output.err;
    EXPECT_EQ(output.status, 1) << "should be a clean error, not a crash";
}
#endif

TEST(Harness, MissingWavFileIsReportedNotCrashed) {
    const Output output = run({"definitely-not-here.wav"});
    EXPECT_EQ(output.status, 1) << "should be a clean error";
    EXPECT_TRUE(contains(output.err, "opening")) << "stderr: " << output.err;
}

TEST(Harness, ChannelOutOfRangeIsReported) {
    const Output output = run({"--sine", "1000", "--seconds", "1", "--channel", "3"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "channel 3"));
}

void put_u16(std::string& out, std::uint32_t value) {
    out.push_back(static_cast<char>(value & 0xFF));
    out.push_back(static_cast<char>((value >> 8) & 0xFF));
}

void put_u32(std::string& out, std::uint32_t value) {
    put_u16(out, value & 0xFFFF);
    put_u16(out, value >> 16);
}

// Reads a real WAV off disk, which is the path the REW parity comparison will
// use. Also checks that integer PCM is scaled correctly rather than being off by
// a factor of two.
TEST(Harness, AnalysesAWavFileFromDisk) {
    const std::filesystem::path dir = temp_path("cli-tests");
    std::filesystem::create_directories(dir);
    const std::filesystem::path path = dir / "on-bin-sine-16bit.wav";

    // 16-bit stereo PCM, written by hand: the model's writer is mono only.
    constexpr std::uint32_t kFrames = 48'000;
    std::string bytes = "RIFF";
    put_u32(bytes, 36 + kFrames * 4);
    bytes += "WAVEfmt ";
    put_u32(bytes, 16);
    put_u16(bytes, 1);       // PCM
    put_u16(bytes, 2);       // channels
    put_u32(bytes, 48'000);  // sample rate
    put_u32(bytes, 48'000 * 4);
    put_u16(bytes, 4);   // block align
    put_u16(bytes, 16);  // bits
    bytes += "data";
    put_u32(bytes, kFrames * 4);
    for (std::uint32_t n = 0; n < kFrames; ++n) {
        const double phase = 2.0 * std::numbers::pi * 996.09375 * static_cast<double>(n) / 48'000.0;
        // Left carries the tone at half scale; right is silent, so a channel
        // mix-up cannot pass unnoticed.
        const auto sample = static_cast<std::int16_t>(0.5 * std::sin(phase) * 32767.0);
        put_u16(bytes, static_cast<std::uint16_t>(sample));
        put_u16(bytes, 0);
    }
    std::ofstream(path, std::ios::binary) << bytes;

    const std::string file = path.string();
    const PeakRow left = peak({file, "--window", "flattop", "--channel", "0", "--peak"});
    EXPECT_LT(std::abs(left.frequency - 996.09375f), 0.001f) << "peak at " << left.frequency;
    EXPECT_LT(std::abs(left.level - kHalfScaleDb), 0.05f)
        << "16-bit PCM scaling wrong: " << left.level << " dBFS, expected " << kHalfScaleDb;

    // The silent channel must read at the floor, not pick up the other one.
    const PeakRow silent = peak({file, "--window", "flattop", "--channel", "1", "--peak"});
    EXPECT_LT(silent.level, -80.0f) << "silent channel read " << silent.level << " dBFS";

    std::filesystem::remove_all(dir);
}

// The swept-measurement path through the CLI, not just the module behind it.
TEST(Harness, MeasureDemoRunsEndToEnd) {
    const Output output = run({"--measure-demo"});
    EXPECT_TRUE(output.success()) << "stderr: " << output.err;

    EXPECT_TRUE(contains(output.out, "# constructed:"));
    EXPECT_TRUE(contains(output.out, "# measured:"));
    EXPECT_TRUE(contains(output.out, "direct arrival"));
    EXPECT_TRUE(contains(output.out, "T30"));
    EXPECT_TRUE(contains(output.out, "gated response"));
}

// The gate flag has to reach the measurement, not just parse.
TEST(Harness, TheGateFlagChangesTheReportedLimit) {
    const auto limit = [](const std::string& ms) {
        const Output output = run({"--measure-demo", "--gate", ms});
        EXPECT_TRUE(output.success());
        for (const std::string& line : lines(output.out)) {
            if (contains(line, "valid above")) {
                // "... valid above 167 Hz": the number before the unit.
                const std::size_t hz = line.rfind(" Hz");
                const std::size_t start = line.rfind(' ', hz - 1);
                return std::stof(line.substr(start + 1, hz - start - 1));
            }
        }
        return std::nanf("");
    };
    EXPECT_LT(limit("40"), limit("5")) << "a wider gate should reach lower in frequency";
}

TEST(Harness, MeasureNeedsTwoPaths) {
    const Output output = run({"--measure", "only-one.wav"});
    EXPECT_FALSE(output.success());
    EXPECT_TRUE(contains(output.err, "needs a value"));
}

TEST(Harness, MeasureReportsAMissingFileCleanly) {
    const Output output = run({"--measure", "nope-a.wav", "nope-b.wav"});
    EXPECT_EQ(output.status, 1) << "should be a clean error";
    EXPECT_TRUE(contains(output.err, "opening")) << "stderr: " << output.err;
}

TEST(Harness, BenchRunsAndReportsEveryStage) {
    const Output output = run({"--bench", "0.2"});
    EXPECT_TRUE(output.success());
    EXPECT_TRUE(contains(output.out, "spectrum analysis"));
    EXPECT_TRUE(contains(output.out, "transfer function"));
    EXPECT_TRUE(contains(output.out, "ring soak"));
    EXPECT_TRUE(contains(output.out, "overruns:"));
}

}  // namespace
}  // namespace analyzer::cli

#endif  // !defined(_WIN32)
