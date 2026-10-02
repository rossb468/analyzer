// Ported from crates/analyzer-model/src/format.rs.

#include "model/format.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "model/error.hpp"
#include "test_util.hpp"

namespace analyzer::model {
namespace {

using test::to_bytes;
using test::to_string;

Measurement spectrum_measurement() {
    std::vector<Complex64> bins;
    for (int k = 0; k < 64; ++k) {
        bins.emplace_back(k * 0.1, -k * 0.05);
    }
    Measurement m(MeasurementId{7}, "Living room, left", 48'000.0,
                  SpectrumData{std::move(bins), 11.71875});
    m.notes = "Mic at listening position";
    m.captured_at = 1'753'800'000;
    m.channels = 2;
    m.references = References{
        .spl_offset_db = 134.25,
        .full_scale_input_volts = 1.23,
        .full_scale_output_volts = std::nullopt,
        .reference_resistance_ohms = std::nullopt,
        .propagation_delay_seconds = 0.0143,
    };
    return m;
}

TEST(Format, ASpectrumRoundTripsExactly) {
    const Measurement original = spectrum_measurement();
    const Measurement restored = read_measurement(write_measurement(original));
    EXPECT_EQ(restored, original);
}

TEST(Format, AnImpulseResponseRoundTripsExactly) {
    std::vector<double> samples;
    for (int n = 0; n < 1000; ++n) {
        samples.push_back(std::sin(n * 0.01));
    }
    Measurement original(MeasurementId{3}, "IR", 96'000.0,
                         ImpulseResponseData{std::move(samples), 412.375});
    original.references.propagation_delay_seconds = 0.0043;

    const Measurement restored = read_measurement(write_measurement(original));
    EXPECT_EQ(restored, original);
}

TEST(Format, ATransferFunctionRoundTripsWithItsCoherence) {
    std::vector<Complex64> bins;
    std::vector<double> coherence;
    for (int k = 0; k < 32; ++k) {
        bins.emplace_back(1.0, k * 0.01);
        coherence.push_back(k / 32.0);
    }
    const Measurement original(MeasurementId{9}, "TF", 44'100.0,
                               TransferFunctionData{std::move(bins), std::move(coherence), 5.38});
    const Measurement restored = read_measurement(write_measurement(original));
    EXPECT_EQ(restored, original);
}

// f64 storage exists so repeated round trips do not accumulate error.
TEST(Format, RepeatedRoundTripsDoNotDrift) {
    Measurement current = spectrum_measurement();
    for (int i = 0; i < 20; ++i) {
        current = read_measurement(write_measurement(current));
    }
    EXPECT_EQ(current, spectrum_measurement());
}

// The point of a text header: a human can identify a file without tooling.
TEST(Format, TheHeaderIsReadableText) {
    const std::vector<std::byte> bytes = write_measurement(spectrum_measurement());
    const std::string text = to_string(std::span(bytes).first(200));
    EXPECT_TRUE(text.starts_with("ANLZ1\n"));
    EXPECT_NE(text.find("name: Living room, left"), std::string::npos);
    EXPECT_NE(text.find("sample_rate: 48000"), std::string::npos);
    EXPECT_NE(text.find("kind: spectrum"), std::string::npos);
}

// A file from a newer version must still load whatever this version knows.
TEST(Format, UnknownHeaderKeysAreIgnored) {
    const std::string text = to_string(write_measurement(spectrum_measurement()));
    const std::size_t at = text.find("---\n");
    ASSERT_NE(at, std::string::npos);
    const std::string patched =
        text.substr(0, at) + "future_field: 42\nanother: hello\n---\n" + text.substr(at + 4);

    const Measurement restored = read_measurement(to_bytes(patched));
    EXPECT_EQ(restored.name, "Living room, left");
}

// Absent must stay absent. Writing a placeholder would turn "not measured"
// into "measured as zero" on the next read.
TEST(Format, UnknownReferencesDoNotBecomeZero) {
    const Measurement m(MeasurementId{1}, "bare", 48'000.0, ImpulseResponseData{{1.0, 2.0}, 0.0});
    EXPECT_TRUE(m.references.is_empty());

    const Measurement restored = read_measurement(write_measurement(m));
    EXPECT_TRUE(restored.references.is_empty());
    EXPECT_EQ(restored.references.spl_offset_db, std::nullopt);
}

TEST(Format, AZeroOffsetSurvivesAsZeroNotNone) {
    Measurement m = spectrum_measurement();
    m.references.spl_offset_db = 0.0;
    const Measurement restored = read_measurement(write_measurement(m));
    EXPECT_EQ(restored.references.spl_offset_db, std::optional(0.0));
}

TEST(Format, NewlinesInNotesSurvive) {
    Measurement m = spectrum_measurement();
    m.notes = "line one\nline two\\with a backslash";
    const Measurement restored = read_measurement(write_measurement(m));
    EXPECT_EQ(restored.notes, m.notes);
}

TEST(Format, APowerSpectrumRoundTrips) {
    std::vector<double> magnitude;
    for (int k = 0; k < 128; ++k) {
        magnitude.push_back(-100.0 + k * 0.37);
    }
    Measurement original(MeasurementId{11}, "RTA capture", 48'000.0,
                         PowerSpectrumData{std::move(magnitude), 11.71875});
    original.references.spl_offset_db = 112.5;
    EXPECT_EQ(read_measurement(write_measurement(original)), original);
}

TEST(Format, AForeignFileIsRejectedByMagic) {
    EXPECT_THROW(read_measurement(to_bytes("RIFF....\n---\n")), BadMagicError);
}

TEST(Format, AHeaderWithoutASeparatorIsRejected) {
    EXPECT_THROW(read_measurement(to_bytes("ANLZ1\nname: x\n")), MissingSeparatorError);
}

TEST(Format, ATruncatedDataBlockIsDetected) {
    const std::vector<std::byte> bytes = write_measurement(spectrum_measurement());
    const std::span<const std::byte> short_file = std::span(bytes).first(bytes.size() - 100);
    EXPECT_THROW(read_measurement(short_file), TruncatedError);
}

TEST(Format, AMissingRequiredFieldIsReportedByName) {
    const std::string text = to_string(write_measurement(spectrum_measurement()));
    // Drop the sample_rate line, keeping everything else byte for byte.
    const std::size_t start = text.find("sample_rate:");
    ASSERT_NE(start, std::string::npos);
    const std::size_t end = text.find('\n', start);
    const std::string stripped = text.substr(0, start) + text.substr(end + 1);

    try {
        read_measurement(to_bytes(stripped));
        FAIL() << "expected a missing field error";
    } catch (const MissingFieldError& error) {
        EXPECT_EQ(error.field(), "sample_rate");
        EXPECT_STREQ(error.what(), "header is missing 'sample_rate'");
    }
}

TEST(Format, AnUnknownKindIsReported) {
    const std::string text = "ANLZ1\nkind: hologram\npoints: 0\nsample_rate: 48000\n---\n";
    EXPECT_THROW(read_measurement(to_bytes(text)), UnknownKindError);
}

TEST(Format, AnEmptyMeasurementRoundTrips) {
    const Measurement m(MeasurementId{0}, "", 48'000.0, SpectrumData{{}, 1.0});
    EXPECT_EQ(read_measurement(write_measurement(m)), m);
}

// The data block must start on a known offset and be contiguous, which is what
// allows a large impulse response to be memory-mapped.
TEST(Format, TheDataBlockIsContiguousAfterTheSeparator) {
    const std::vector<std::byte> bytes = write_measurement(spectrum_measurement());
    const std::string_view separator = "\n---\n";
    const auto found = std::ranges::search(bytes, std::as_bytes(std::span(separator)));
    ASSERT_FALSE(found.empty());
    const auto start = static_cast<std::size_t>(found.end() - bytes.begin());
    EXPECT_EQ(bytes.size() - start, 64u * 16u) << "64 complex values, 16 bytes each";
}

// The error messages carry what the Rust Display did, including the Debug
// quoting of the offending text.
TEST(Format, ErrorMessagesSayWhatWentWrong) {
    const auto message = [](std::string_view text) -> std::string {
        try {
            read_measurement(to_bytes(text));
        } catch (const ModelError& error) {
            return error.what();
        }
        return "no error";
    };
    EXPECT_EQ(message("RIFF....\n---\n"),
              "not an analyzer measurement file (magic was \"RIFF....\")");
    EXPECT_EQ(message("ANLZ1\nname: x\n"), "header has no '---' separator");
    EXPECT_EQ(message("ANLZ1\nkind: spectrum\npoints: many\nsample_rate: 48000\n---\n"),
              "cannot parse 'points' from \"many\"");
    EXPECT_EQ(message("ANLZ1\nkind: hologram\npoints: 0\nsample_rate: 48000\n---\n"),
              "unknown measurement kind \"hologram\"");
    EXPECT_EQ(message("ANLZ1\nkind: spectrum\npoints: 2\nsample_rate: 48000\n---\n"),
              "data block truncated: expected 32 bytes, found 0");
}

// A header that claims more points than the address space can hold must read as
// truncated, not overflow its byte count into a small number.
TEST(Format, AnAbsurdPointCountIsTruncatedNotOverflowed) {
    const std::string text =
        "ANLZ1\nkind: spectrum\npoints: 1e300\nsample_rate: 48000\nbin_spacing_hz: 1\n---\n";
    EXPECT_THROW(read_measurement(to_bytes(text)), TruncatedError);
}

// Optional keys that do not parse read as unknown, where required ones throw.
TEST(Format, UnparseableOptionalKeysReadAsUnknown) {
    const std::string text =
        "ANLZ1\nkind: spectrum\npoints: 0\nsample_rate: 48000\nbin_spacing_hz: 1\n"
        "spl_offset_db: loud\nchannels: two\n---\n";
    const Measurement m = read_measurement(to_bytes(text));
    EXPECT_EQ(m.references.spl_offset_db, std::nullopt);
    EXPECT_EQ(m.channels, 1u);
}

}  // namespace
}  // namespace analyzer::model
