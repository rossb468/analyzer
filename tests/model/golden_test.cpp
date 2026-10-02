// Checks against the files the Rust core wrote.
//
// tests/golden/fixtures/ holds reference outputs recorded from the Rust build,
// from the exact inputs hard-coded in tests/golden/rust/src/main.rs. Every input
// is rebuilt here from the same numbers, and what this code writes is compared
// with what the Rust wrote. See tests/golden/README.md for the rules each class
// of file is held to.
//
// Saved measurements (.anlz), the filter exports for REW and Equalizer APO, the
// settings files and the WAV headers must be byte-identical. Where a value
// passes through a transcendental function the README allows the last printed
// digit to differ between math libraries, and these tests allow exactly that
// and no more.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "dsp/eq.hpp"
#include "dsp/generator.hpp"
#include "model/export.hpp"
#include "model/filter_export.hpp"
#include "model/format.hpp"
#include "model/measurement.hpp"
#include "model/settings.hpp"
#include "model/wav.hpp"
#include "test_util.hpp"

#ifndef ANALYZER_GOLDEN_DIR
#error "ANALYZER_GOLDEN_DIR must point at tests/golden/fixtures"
#endif

namespace analyzer::model {
namespace {

using dsp::FilterBand;
using dsp::FilterKind;
using dsp::Signal;
using test::read_bytes;
using test::read_text;
using test::scratch;

const std::filesystem::path kFixtures = ANALYZER_GOLDEN_DIR;

std::filesystem::path model_fixture(std::string_view name) {
    return kFixtures / "model" / name;
}

// Exactly representable ramp in [-0.5, 0.5): ((i * mul) % 64) / 64 - 0.5.
double ramp(std::size_t i, std::size_t mul) {
    return static_cast<double>((i * mul) % 64) / 64.0 - 0.5;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t end = text.find('\n', start);
        out.push_back(text.substr(start, end == std::string::npos ? end : end - start));
        if (end == std::string::npos) {
            break;
        }
        start = end + 1;
    }
    return out;
}

std::vector<std::string> split_fields(const std::string& line) {
    std::vector<std::string> out;
    std::size_t start = 0;
    while (start < line.size()) {
        const std::size_t end = std::min(line.find(' ', start), line.size());
        if (end > start) {
            out.push_back(line.substr(start, end - start));
        }
        start = end + 1;
    }
    return out;
}

// -- Saved measurements ------------------------------------------------------

// One measurement of each kind, as the fixture writer builds them.
Measurement golden_impulse_response() {
    std::vector<double> samples;
    for (std::size_t i = 0; i < 256; ++i) {
        samples.push_back(ramp(i, 37));
    }
    // Every reference set, captured_at pinned, free text with a newline and a
    // backslash so the header escaping is covered.
    Measurement m(MeasurementId{7}, "Living room, left\nsecond line", 48'000.0,
                  ImpulseResponseData{std::move(samples), 12.5});
    m.notes = "mic at 1 m\\on axis\nline two";
    m.captured_at = 1'700'000'000;
    m.channels = 2;
    m.references.spl_offset_db = 134.25;
    m.references.full_scale_input_volts = 1.5;
    m.references.full_scale_output_volts = 2.0;
    m.references.reference_resistance_ohms = 10.0;
    m.references.propagation_delay_seconds = 0.0078125;
    return m;
}

// Complex spectrum, SPL calibrated: levels are bin magnitude plus offset. Bin 0
// is zero magnitude so the -200 dB floor is covered.
Measurement golden_spectrum() {
    std::vector<Complex64> bins;
    for (std::size_t k = 0; k < 64; ++k) {
        bins.push_back(k == 0 ? Complex64(0.0, 0.0) : Complex64(ramp(k, 5) * 2.0, ramp(k, 11)));
    }
    Measurement m(MeasurementId{8}, "Spectrum", 48'000.0, SpectrumData{std::move(bins), 750.0});
    m.captured_at = 1'700'000'001;
    m.references.spl_offset_db = 94.0;
    return m;
}

// Magnitude-only, no references at all: the "not calibrated" branch, and the
// proof None is written as an absent line rather than zero.
Measurement golden_power_spectrum() {
    std::vector<double> magnitude;
    for (std::size_t k = 0; k < 32; ++k) {
        magnitude.push_back(-20.0 - ramp(k, 3) * 40.0);
    }
    return Measurement(MeasurementId{9}, "Power spectrum", 44'100.0,
                       PowerSpectrumData{std::move(magnitude), 1378.125});
}

// Two-channel transfer function with coherence; propagation delay set so the
// export's "delay removed" header line is covered.
Measurement golden_transfer_function() {
    std::vector<Complex64> bins;
    std::vector<double> coherence;
    for (std::size_t k = 0; k < 32; ++k) {
        bins.emplace_back(1.0 + ramp(k, 7), ramp(k, 13));
        coherence.push_back(0.5 + static_cast<double>(k % 8) / 16.0);
    }
    Measurement m(MeasurementId{10}, "Transfer", 48'000.0,
                  TransferFunctionData{std::move(bins), std::move(coherence), 93.75});
    m.captured_at = 1'700'000'002;
    m.channels = 2;
    m.references.propagation_delay_seconds = 0.005;
    m.references.spl_offset_db = 0.0;  // measured as needing none
    return m;
}

struct GoldenMeasurement {
    const char* stem;
    Measurement (*build)();
    // Whether the REW text may differ from the fixture in the last printed digit
    // of its level, phase and coherence columns. False where the values never
    // pass through a transcendental function.
    bool libm_dependent;
};

constexpr GoldenMeasurement kMeasurements[] = {
    {"measurement_impulse_response", golden_impulse_response, false},
    {"measurement_spectrum", golden_spectrum, true},
    {"measurement_power_spectrum", golden_power_spectrum, true},
    {"measurement_transfer_function", golden_transfer_function, true},
};

TEST(GoldenMeasurement, WritingGivesTheFixtureByteForByte) {
    for (const GoldenMeasurement& golden : kMeasurements) {
        const std::vector<std::byte> expected =
            read_bytes(model_fixture(std::string(golden.stem) + ".anlz"));
        const std::vector<std::byte> actual = write_measurement(golden.build());
        EXPECT_EQ(actual == expected, true) << golden.stem << ": the .anlz bytes differ";
    }
}

TEST(GoldenMeasurement, ReadingTheFixtureReproducesTheValues) {
    for (const GoldenMeasurement& golden : kMeasurements) {
        const std::vector<std::byte> bytes =
            read_bytes(model_fixture(std::string(golden.stem) + ".anlz"));
        EXPECT_EQ(read_measurement(bytes), golden.build()) << golden.stem;
    }
}

// What the Rust fixture writer asserts, and the contract of the container: a
// file read and written again comes out the same bytes.
TEST(GoldenMeasurement, ReadThenWriteGivesTheSameBytes) {
    for (const GoldenMeasurement& golden : kMeasurements) {
        const std::vector<std::byte> bytes =
            read_bytes(model_fixture(std::string(golden.stem) + ".anlz"));
        EXPECT_EQ(write_measurement(read_measurement(bytes)) == bytes, true) << golden.stem;
    }
}

// The power spectrum has no references at all, and that has to be absent lines
// rather than zeros; the transfer function has an offset of zero, which has to
// be present.
TEST(GoldenMeasurement, AbsentReferencesAreAbsentAndZeroIsKept) {
    const Measurement power =
        read_measurement(read_bytes(model_fixture("measurement_power_spectrum.anlz")));
    EXPECT_TRUE(power.references.is_empty());
    const Measurement transfer =
        read_measurement(read_bytes(model_fixture("measurement_transfer_function.anlz")));
    EXPECT_EQ(transfer.references.spl_offset_db, std::optional(0.0));
    EXPECT_TRUE(transfer.is_spl_calibrated());
}

// Header and impulse-response rows are pure arithmetic on exact values and must
// match byte for byte. Spectrum rows pass through log10 and atan2, so their
// level, phase and coherence columns may differ in the last printed digit between
// math libraries (0.0001); the frequency column and every comment line are exact.
void expect_rew_text_matches(const std::string& actual, const std::string& expected,
                             bool libm_dependent, const char* stem) {
    if (!libm_dependent) {
        EXPECT_EQ(actual, expected) << stem;
        return;
    }
    const std::vector<std::string> actual_lines = split_lines(actual);
    const std::vector<std::string> expected_lines = split_lines(expected);
    ASSERT_EQ(actual_lines.size(), expected_lines.size()) << stem;
    for (std::size_t i = 0; i < actual_lines.size(); ++i) {
        if (expected_lines[i].starts_with('*')) {
            ASSERT_EQ(actual_lines[i], expected_lines[i]) << stem << " line " << i;
            continue;
        }
        const auto got = split_fields(actual_lines[i]);
        const auto want = split_fields(expected_lines[i]);
        ASSERT_EQ(got.size(), want.size()) << stem << " line " << i;
        ASSERT_EQ(got[0], want[0]) << stem << ": frequency column, line " << i;
        for (std::size_t c = 1; c < got.size(); ++c) {
            if (want[c] == "*") {
                ASSERT_EQ(got[c], want[c]) << stem << " line " << i;
                continue;
            }
            ASSERT_NEAR(std::stod(got[c]), std::stod(want[c]), 1.1e-4)
                << stem << " line " << i << " column " << c << ": '" << actual_lines[i]
                << "' against '" << expected_lines[i] << "'";
        }
    }
}

TEST(GoldenMeasurement, RewTextMatchesTheFixture) {
    for (const GoldenMeasurement& golden : kMeasurements) {
        const std::string expected =
            read_text(model_fixture(std::string(golden.stem) + ".rew.txt"));
        expect_rew_text_matches(to_rew_text(golden.build()), expected, golden.libm_dependent,
                                golden.stem);
    }
}

// The 0 Hz row of the spectrum prints the floor: zero magnitude is floored and
// the SPL offset is not added to it.
TEST(GoldenMeasurement, TheSilentBinPrintsTheFloorWithoutTheOffset) {
    const std::string text = to_rew_text(golden_spectrum());
    EXPECT_NE(text.find("\n0.000000 -200.0000 0.0000\n"), std::string::npos);
}

// -- Filter exports ----------------------------------------------------------

// Ten bands covering every shape, a disabled slot, and a transparent band.
std::vector<FilterBand> filter_bands() {
    const auto band = [](FilterKind kind, float hz, float gain_db, float q, bool enabled) {
        return FilterBand{kind, hz, gain_db, q, enabled};
    };
    return {
        band(FilterKind::Peaking, 63.0f, -5.5f, 4.0f, true),
        band(FilterKind::LowShelf, 100.0f, 3.0f, 0.707f, true),
        band(FilterKind::HighShelf, 8000.0f, -2.5f, 0.5f, true),
        band(FilterKind::HighPass, 20.0f, 0.0f, 0.707f, true),
        band(FilterKind::LowPass, 18000.0f, 0.0f, 0.707f, true),
        band(FilterKind::BandPass, 1000.0f, 0.0f, 2.0f, true),
        band(FilterKind::Notch, 50.0f, 0.0f, 10.0f, true),
        band(FilterKind::AllPass, 500.0f, 0.0f, 1.0f, true),
        band(FilterKind::Peaking, 250.0f, 4.0f, 1.5f, false),  // present but OFF
        band(FilterKind::Peaking, 1000.0f, 0.0f, 1.5f, true),  // transparent
    };
}

constexpr float kPreamp = -6.25f;

// REW and APO text is byte-exact. The two formatting traps the README records
// are both here: -6.25 at one place prints -6.2 (an exact tie rounds to even),
// and APO prints f32 values in their shortest form (63, 0.707, -5.5).
TEST(GoldenFilters, RewAndApoTextIsByteIdentical) {
    const std::vector<FilterBand> bands = filter_bands();
    EXPECT_EQ(to_text(FilterFormat::Rew, bands, kPreamp, 48'000.0f),
              read_text(model_fixture("filters_all_kinds.rew.txt")));
    EXPECT_EQ(to_text(FilterFormat::EqualizerApo, bands, kPreamp, 48'000.0f),
              read_text(model_fixture("filters_all_kinds.apo.txt")));
}

TEST(GoldenFilters, EmptyEqualiserTextIsByteIdentical) {
    EXPECT_EQ(to_text(FilterFormat::Rew, {}, -3.0f, 48'000.0f),
              read_text(model_fixture("filters_empty.rew.txt")));
    EXPECT_EQ(to_text(FilterFormat::EqualizerApo, {}, -3.0f, 48'000.0f),
              read_text(model_fixture("filters_empty.apo.txt")));
}

// miniDSP coefficients are the designed f32 printed to 15 decimals, so the last
// digits follow whatever the filter design arithmetic rounds to. Block structure,
// labels and commas are exact; the numbers agree to 1e-6.
void expect_minidsp_matches(const std::string& actual, const std::string& expected,
                            const char* name) {
    const std::vector<std::string> actual_lines = split_lines(actual);
    const std::vector<std::string> expected_lines = split_lines(expected);
    ASSERT_EQ(actual_lines.size(), expected_lines.size()) << name;
    for (std::size_t i = 0; i < actual_lines.size(); ++i) {
        const std::string& want = expected_lines[i];
        const std::string& got = actual_lines[i];
        const std::size_t equals = want.find('=');
        if (equals == std::string::npos) {
            ASSERT_EQ(got, want) << name << " line " << i;
            continue;
        }
        const std::size_t got_equals = got.find('=');
        ASSERT_NE(got_equals, std::string::npos) << name << " line " << i << ": " << got;
        ASSERT_EQ(got.substr(0, got_equals), want.substr(0, equals)) << name << " line " << i;
        const bool want_comma = want.ends_with(',');
        ASSERT_EQ(got.ends_with(','), want_comma) << name << " line " << i << ": " << got;
        const double want_value = std::stod(want.substr(equals + 1));
        const double got_value = std::stod(got.substr(got_equals + 1));
        ASSERT_NEAR(got_value, want_value, 1e-6) << name << " line " << i;
        // Negated zero keeps its sign in the Rust output; so must this.
        ASSERT_EQ(std::signbit(got_value), std::signbit(want_value))
            << name << " line " << i << ": " << got << " against " << want;
    }
}

TEST(GoldenFilters, MiniDspAtTwoSampleRates) {
    const std::vector<FilterBand> bands = filter_bands();
    expect_minidsp_matches(to_text(FilterFormat::MiniDsp, bands, kPreamp, 48'000.0f),
                           read_text(model_fixture("filters_all_kinds.minidsp_48k.txt")), "48k");
    expect_minidsp_matches(to_text(FilterFormat::MiniDsp, bands, kPreamp, 96'000.0f),
                           read_text(model_fixture("filters_all_kinds.minidsp_96k.txt")), "96k");
}

// No bands: miniDSP must still carry the trim, as a gain-only section, and
// a1 and a2 print as -0.000000000000000 because negated zero keeps its sign.
TEST(GoldenFilters, MiniDspCarriesTheTrimWithNoBands) {
    const std::string text = to_text(FilterFormat::MiniDsp, {}, -3.0f, 48'000.0f);
    expect_minidsp_matches(text, read_text(model_fixture("filters_empty.minidsp_48k.txt")),
                           "empty");
    EXPECT_NE(text.find("a1=-0.000000000000000,"), std::string::npos);
    EXPECT_NE(text.find("a2=-0.000000000000000\n"), std::string::npos);
}

// -- Settings ----------------------------------------------------------------

TEST(GoldenSettings, DefaultsAreByteIdentical) {
    EXPECT_EQ(Settings{}.to_text(), read_text(model_fixture("settings_default.cfg")));
}

TEST(GoldenSettings, ACustomSetIsByteIdentical) {
    Settings custom;
    custom.fft_size = 16'384;
    custom.window = WindowChoice::FlatTop;
    custom.averaging = AveragingChoice::PeakHold;
    custom.start_on_launch = false;
    custom.min_hz = 10.0f;
    custom.max_hz = 24'000.0f;
    custom.min_db = -100.0f;
    custom.max_db = 10.0f;
    custom.level_grid_step = 10.0f;
    custom.spl_offset_db = 94.3f;
    custom.mic_cal_path = "/Users/test/cal/umik-1.txt";
    const std::string text = custom.to_text();
    EXPECT_EQ(text, read_text(model_fixture("settings_custom.cfg")));
    EXPECT_EQ(Settings::from_text(text), custom);
}

// A damaged file and what the infallible parser makes of it: the unknown key is
// dropped, bad values fall back per field, FFT 3000 becomes 4096, the inverted
// axes are repaired, a zero offset is kept (zero is not "none"), and the empty
// path is dropped.
TEST(GoldenSettings, AHandEditedFileIsRepairedTheSameWay) {
    const std::string input = read_text(model_fixture("settings_hand_edited.in.cfg"));
    const std::string expected = read_text(model_fixture("settings_hand_edited.out.cfg"));
    EXPECT_EQ(Settings::from_text(input).to_text(), expected);

    const Settings repaired = Settings::from_text(input);
    EXPECT_EQ(repaired.fft_size, 4096u);
    EXPECT_EQ(repaired.window, WindowChoice::Hann) << "kaiser is not a window";
    EXPECT_EQ(repaired.spl_offset_db, std::optional(0.0f));
    EXPECT_EQ(repaired.mic_cal_path, std::nullopt);
    EXPECT_GT(repaired.max_hz, repaired.min_hz);
    EXPECT_GT(repaired.max_db, repaired.min_db);
}

// The hand-edited input is also the fixture writer's own string; this is the
// text of it, so the test above is not the only thing that depends on the file.
TEST(GoldenSettings, TheHandEditedInputIsWhatTheFixtureWriterUsed) {
    const std::string expected =
        "ANLZCFG1\n"
        "# edited by hand\n"
        "future_key: 42\n"
        "fft_size: 3000\n"
        "window: kaiser\n"
        "averaging: infinite\n"
        "start_on_launch: maybe\n"
        "min_hz: 5000\n"
        "max_hz: 100\n"
        "min_db: -80\n"
        "max_db: -90\n"
        "level_grid_step: 0\n"
        "spl_offset_db: 0\n"
        "mic_cal_path:\n";
    EXPECT_EQ(read_text(model_fixture("settings_hand_edited.in.cfg")), expected);
}

// -- WAV files ---------------------------------------------------------------

struct GoldenWav {
    const char* file;
    Signal signal;
    float rate;
    float seconds;
    SampleDepth depth;
};

// The `--generate` cases in MANIFEST.tsv, with the CLI's defaults: amplitude 0.5,
// a 1 kHz sine, a sweep from 20 Hz to 20 kHz, and a sweep that is a single pass
// of exactly the requested seconds.
const GoldenWav kWavs[] = {
    {"sine_i16.wav", Signal::sine(1000.0f, 0.5f), 48'000.0f, 0.5f, SampleDepth::Int16},
    {"sine_i24.wav", Signal::sine(1000.0f, 0.5f), 48'000.0f, 0.5f, SampleDepth::Int24},
    {"sine_f32.wav", Signal::sine(1000.0f, 0.5f), 48'000.0f, 0.5f, SampleDepth::Float32},
    {"pink_i16.wav", Signal::pink_noise(0.5f), 48'000.0f, 0.5f, SampleDepth::Int16},
    {"pink_i24.wav", Signal::pink_noise(0.5f), 48'000.0f, 0.5f, SampleDepth::Int24},
    {"pink_f32.wav", Signal::pink_noise(0.5f), 48'000.0f, 0.5f, SampleDepth::Float32},
    {"white_i16.wav", Signal::white_noise(0.5f), 48'000.0f, 0.5f, SampleDepth::Int16},
    {"white_i24.wav", Signal::white_noise(0.5f), 48'000.0f, 0.5f, SampleDepth::Int24},
    {"white_f32.wav", Signal::white_noise(0.5f), 48'000.0f, 0.5f, SampleDepth::Float32},
    {"sweep_i16.wav", Signal::sweep(20.0f, 20'000.0f, 0.5f, 0.5f, false), 48'000.0f, 0.5f,
     SampleDepth::Int16},
    {"sweep_i24.wav", Signal::sweep(20.0f, 20'000.0f, 0.5f, 0.5f, false), 48'000.0f, 0.5f,
     SampleDepth::Int24},
    {"sweep_f32.wav", Signal::sweep(20.0f, 20'000.0f, 0.5f, 0.5f, false), 48'000.0f, 0.5f,
     SampleDepth::Float32},
    {"sine440_44k1_i24.wav", Signal::sine(440.0f, 0.25f), 44'100.0f, 0.5f, SampleDepth::Int24},
    {"sweep_100_5000_i16.wav", Signal::sweep(100.0f, 5000.0f, 0.5f, 0.5f, false), 48'000.0f, 0.5f,
     SampleDepth::Int16},
    {"sweep_1s_f32.wav", Signal::sweep(20.0f, 20'000.0f, 1.0f, 0.5f, false), 48'000.0f, 1.0f,
     SampleDepth::Float32},
    {"sweep_44k1_i16.wav", Signal::sweep(20.0f, 20'000.0f, 0.25f, 0.5f, false), 44'100.0f, 0.25f,
     SampleDepth::Int16},
};

std::size_t header_bytes(SampleDepth depth) {
    return depth == SampleDepth::Int16 ? 44 : 68;
}

// Every byte before the sample payload is exact: the RIFF and data sizes, the
// format tag, and for 24-bit and float the 68-byte extensible header with no
// fact chunk.
void expect_same_header(const std::vector<std::byte>& actual,
                        const std::vector<std::byte>& expected, SampleDepth depth,
                        const std::string& name) {
    ASSERT_EQ(actual.size(), expected.size()) << name << ": file size";
    const std::size_t header = header_bytes(depth);
    ASSERT_GE(actual.size(), header) << name;
    EXPECT_TRUE(std::equal(actual.begin(), actual.begin() + static_cast<std::ptrdiff_t>(header),
                           expected.begin()))
        << name << ": header bytes differ";
}

// Sample payloads: integer files within one least significant bit, float files
// within 1e-6, as the README sets for the generated audio. White noise is integer
// arithmetic only and comes out bit-exact; the others use sin, exp and an f32
// filter chain.
void expect_payload_close(const WavFile& actual, const WavFile& expected, const std::string& name) {
    ASSERT_EQ(actual.samples.size(), expected.samples.size()) << name;
    const float lsb =
        expected.is_float ? 1e-6f : 1.0f / std::ldexp(1.0f, expected.bits_per_sample - 1);
    for (std::size_t i = 0; i < actual.samples.size(); ++i) {
        ASSERT_NEAR(actual.samples[i], expected.samples[i], expected.is_float ? lsb : lsb * 1.01f)
            << name << " sample " << i;
    }
}

TEST(GoldenWav, GeneratedFilesMatchTheFixtures) {
    for (const GoldenWav& golden : kWavs) {
        const std::filesystem::path fixture = kFixtures / "wav" / golden.file;
        const std::filesystem::path path = scratch(golden.file);
        const std::size_t frames =
            write_signal(path, golden.signal, golden.rate, golden.seconds, golden.depth);

        const std::vector<std::byte> expected = read_bytes(fixture);
        expect_same_header(read_bytes(path), expected, golden.depth, golden.file);
        const WavFile actual = read_wav(path);
        const WavFile want = read_wav(fixture);
        EXPECT_EQ(actual.frames(), frames) << golden.file;
        EXPECT_EQ(actual.sample_rate, static_cast<double>(golden.rate)) << golden.file;
        expect_payload_close(actual, want, golden.file);
        std::filesystem::remove(path);
    }
}

TEST(GoldenWav, WhiteNoiseIsBitExact) {
    for (const GoldenWav& golden : kWavs) {
        if (golden.signal.kind != Signal::Kind::WhiteNoise) {
            continue;
        }
        const std::filesystem::path path = scratch(golden.file);
        write_signal(path, golden.signal, golden.rate, golden.seconds, golden.depth);
        EXPECT_EQ(read_bytes(path) == read_bytes(kFixtures / "wav" / golden.file), true)
            << golden.file;
        std::filesystem::remove(path);
    }
}

// The one input WAV the CLI cannot make: the 1 s sweep played through four
// discrete arrivals, accumulated in a fixed order in f32 - (delay in samples,
// gain) of (240, 0.5), (336, 0.3), (768, -0.2) and (1440, 0.1), length n + 1441.
TEST(GoldenWav, TheSyntheticRoomResponseMatchesTheFixture) {
    constexpr float kRate = 48'000.0f;
    const std::vector<float> stimulus =
        render(Signal::sweep(20.0f, 20'000.0f, 1.0f, 0.5f, false), kRate, 1.0f);

    struct Arrival {
        std::size_t delay;
        float gain;
    };
    constexpr Arrival kArrivals[] = {{240, 0.5f}, {336, 0.3f}, {768, -0.2f}, {1440, 0.1f}};
    std::vector<float> response(stimulus.size() + 1440 + 1, 0.0f);
    for (const Arrival& arrival : kArrivals) {
        for (std::size_t n = 0; n < stimulus.size(); ++n) {
            response[n + arrival.delay] += stimulus[n] * arrival.gain;
        }
    }

    const std::filesystem::path fixture = kFixtures / "wav" / "response_room_f32.wav";
    const std::filesystem::path path = scratch("response_room_f32.wav");
    write_wav(path, response, kRate, SampleDepth::Float32);

    expect_same_header(read_bytes(path), read_bytes(fixture), SampleDepth::Float32,
                       "response_room_f32.wav");
    expect_payload_close(read_wav(path), read_wav(fixture), "response_room_f32.wav");
    EXPECT_EQ(response.size(), stimulus.size() + 1441u);
    std::filesystem::remove(path);
}

// The reader accepts every file the writer produced: the 17 fixtures cover
// 16-bit, 24-bit and float, at two sample rates.
TEST(GoldenWav, EveryFixtureReads) {
    std::size_t read = 0;
    for (const auto& entry : std::filesystem::directory_iterator(kFixtures / "wav")) {
        const WavFile file = read_wav(entry.path());
        EXPECT_EQ(file.channels, 1u) << entry.path();
        EXPECT_GT(file.frames(), 0u) << entry.path();
        EXPECT_TRUE(file.sample_rate == 48'000.0 || file.sample_rate == 44'100.0) << entry.path();
        EXPECT_TRUE(std::ranges::all_of(file.samples, [](float s) { return std::abs(s) <= 1.0f; }))
            << entry.path();
        ++read;
    }
    EXPECT_EQ(read, 17u);
}

}  // namespace
}  // namespace analyzer::model
