// The C++ analyzer-cli against what the Rust analyzer-cli printed.
//
// tests/golden/fixtures/MANIFEST.tsv records 61 invocations of the Rust CLI:
// its arguments, exit status, stdout, stderr and any file it wrote. Each row
// becomes one test here. The built executable is run with the recorded
// arguments in a scratch copy of the fixtures directory (the working directory
// matters: paths in the arguments are relative to it, and --measure echoes
// them into its report), and the result is compared under the row's class and
// the tolerances in tests/golden/README.md. Read that document for why each
// tolerance is what it is; the comments below say only how they are applied.
//
// A failure message always carries the measured difference, so a tolerance is
// widened only with a number in hand and a written reason.

#if !defined(_WIN32)

#include <gtest/gtest.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "cli/text.hpp"
#include "subprocess.hpp"

namespace analyzer::cli {
namespace {

namespace fs = std::filesystem;

const fs::path kFixtures = ANALYZER_GOLDEN_DIR;
const std::string kBin = ANALYZER_CLI_PATH;

// Printed values are rounded, so two outputs one unit in the last place apart
// differ by exactly the tolerance; this keeps that case on the passing side of
// a floating-point comparison.
constexpr double kSlack = 1e-9;

// --------------------------------------------------------------- manifest --

struct Row {
    std::string id;
    int exit_status = 0;
    std::string kind;
    std::string stdout_file;
    std::string stderr_file;
    std::string artifact;
    std::vector<std::string> args;

    bool has_arg(std::string_view flag) const {
        return std::ranges::find(args, flag) != args.end();
    }
};

std::string read_file(const fs::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        ADD_FAILURE() << "cannot read " << path;
        return {};
    }
    std::ostringstream text;
    text << file.rdbuf();
    return text.str();
}

std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> pieces;
    std::size_t start = 0;
    while (true) {
        const std::size_t end = text.find(separator, start);
        if (end == std::string::npos) {
            pieces.push_back(text.substr(start));
            return pieces;
        }
        pieces.push_back(text.substr(start, end - start));
        start = end + 1;
    }
}

// Rows, in file order. Tab separated; the last column (the arguments) is empty
// for the no-arguments case, so empty fields are kept.
std::vector<Row> load_manifest() {
    std::vector<Row> rows;
    std::ifstream file(kFixtures / "MANIFEST.tsv");
    std::string line;
    std::getline(file, line);  // header
    while (std::getline(file, line)) {
        if (line.empty()) {
            continue;
        }
        const std::vector<std::string> columns = split(line, '\t');
        if (columns.size() != 7) {
            continue;
        }
        Row row;
        row.id = columns[0];
        row.exit_status = std::stoi(columns[1]);
        row.kind = columns[2];
        row.stdout_file = columns[3];
        row.stderr_file = columns[4];
        row.artifact = columns[5];
        // No argument contains a space, so a space is the separator.
        if (!columns[6].empty()) {
            row.args = split(columns[6], ' ');
        }
        rows.push_back(std::move(row));
    }
    return rows;
}

// ------------------------------------------------------------ scratch dir --

// A private copy of the fixtures, so a command that writes into wav/ or files/
// cannot disturb the committed inputs or another test running beside it.
class Scratch {
public:
    explicit Scratch(const std::string& id)
        : path_(fs::temp_directory_path() /
                ("analyzer-golden-" + std::to_string(getpid()) + "-" + id)) {
        fs::remove_all(path_);
        fs::copy(kFixtures, path_, fs::copy_options::recursive);
    }
    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }
    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    const fs::path& path() const noexcept { return path_; }

private:
    fs::path path_;
};

// ------------------------------------------------------------- text tools --

std::vector<std::string> lines_of(const std::string& text) {
    std::vector<std::string> out;
    std::istringstream stream(text);
    for (std::string line; std::getline(stream, line);) {
        out.push_back(line);
    }
    return out;
}

std::vector<std::string> words_of(const std::string& line) {
    std::vector<std::string> out;
    std::istringstream stream(line);
    for (std::string word; stream >> word;) {
        out.push_back(word);
    }
    return out;
}

// A number as the harness prints it, with an optional sign. nullopt if `word`
// is not entirely a number.
std::optional<double> number(const std::string& word) {
    return text::parse_f64(word);
}

std::string strip_suffix(std::string word, char suffix) {
    if (!word.empty() && word.back() == suffix) {
        word.pop_back();
    }
    return word;
}

// ------------------------------------------------------------------ exact --

void expect_exact(const std::string& what, const std::string& expected, const std::string& actual) {
    EXPECT_EQ(actual, expected) << what << " differs";
}

// ----------------------------------------------------------------- wavgen --

struct WavLayout {
    std::size_t payload_offset = 0;
    std::size_t payload_size = 0;
    std::uint32_t format_tag = 0;
    std::uint32_t bits = 0;
};

std::uint32_t le(const std::string& bytes, std::size_t at, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = count; i-- > 0;) {
        value = (value << 8) | static_cast<unsigned char>(bytes.at(at + i));
    }
    return value;
}

// Walk the chunks to find where the sample payload starts. Everything before it
// is "the header", whatever shape the writer gave it.
std::optional<WavLayout> layout_of(const std::string& bytes) {
    if (bytes.size() < 12 || bytes.compare(0, 4, "RIFF") != 0 || bytes.compare(8, 4, "WAVE") != 0) {
        return std::nullopt;
    }
    WavLayout layout;
    std::size_t at = 12;
    while (at + 8 <= bytes.size()) {
        const std::string id = bytes.substr(at, 4);
        const std::size_t size = le(bytes, at + 4, 4);
        if (id == "fmt ") {
            layout.format_tag = le(bytes, at + 8, 2);
            layout.bits = le(bytes, at + 8 + 14, 2);
            if (layout.format_tag == 0xFFFE) {
                // Extensible: the real tag is the first two bytes of the sub-format GUID.
                layout.format_tag = le(bytes, at + 8 + 24, 2);
            }
        } else if (id == "data") {
            layout.payload_offset = at + 8;
            layout.payload_size = size;
            return layout;
        }
        at += 8 + size + (size & 1);
    }
    return std::nullopt;
}

// Header byte-exact; payload within one LSB for integers and 1e-6 for float.
void expect_wav_matches(const std::string& expected, const std::string& actual) {
    const auto want = layout_of(expected);
    const auto got = layout_of(actual);
    ASSERT_TRUE(want.has_value()) << "fixture is not a WAV";
    ASSERT_TRUE(got.has_value()) << "the artifact is not a WAV";

    ASSERT_EQ(got->payload_offset, want->payload_offset) << "header length differs";
    EXPECT_EQ(actual.substr(0, got->payload_offset), expected.substr(0, want->payload_offset))
        << "header bytes differ";
    ASSERT_EQ(actual.size(), expected.size()) << "file size differs";

    const bool is_float = want->format_tag == 3;
    ASSERT_TRUE(is_float || want->format_tag == 1) << "format tag " << want->format_tag;
    const std::size_t width = want->bits / 8;
    ASSERT_GT(width, 0u);

    double worst = 0.0;
    std::size_t worst_at = 0;
    const std::size_t count = want->payload_size / width;
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = want->payload_offset + i * width;
        double difference = 0.0;
        if (is_float) {
            float a = 0.0f;
            float b = 0.0f;
            const std::uint32_t abits = le(actual, at, 4);
            const std::uint32_t bbits = le(expected, at, 4);
            std::memcpy(&a, &abits, 4);
            std::memcpy(&b, &bbits, 4);
            difference = std::abs(static_cast<double>(a) - static_cast<double>(b));
        } else {
            // Sign-extend from the sample's own width.
            const auto widen = [&](const std::string& bytes) {
                std::int64_t value = le(bytes, at, width);
                const std::int64_t sign = std::int64_t{1} << (width * 8 - 1);
                return (value ^ sign) - sign;
            };
            difference = static_cast<double>(std::abs(widen(actual) - widen(expected)));
        }
        if (difference > worst) {
            worst = difference;
            worst_at = i;
        }
    }
    const double tolerance = is_float ? 1e-6 : 1.0;
    EXPECT_LE(worst, tolerance + (is_float ? 0.0 : kSlack))
        << "payload differs by " << worst << (is_float ? "" : " LSB") << " at sample " << worst_at;
}

// --------------------------------------------------------------- spectrum --

// How far the level of a bin may move, by how far below full scale the fixture
// has it: float round-off in the transform sets a floor near -130 dBFS.
// nullopt means "not compared".
std::optional<double> level_tolerance(double expected_db) {
    if (expected_db >= -90.0) {
        return 0.01;
    }
    if (expected_db >= -110.0) {
        return 0.1;
    }
    if (expected_db >= -130.0) {
        return 1.0;
    }
    return std::nullopt;
}

struct SpectrumFile {
    std::vector<std::string> comments;
    std::vector<std::pair<std::string, std::string>> rows;  // frequency text, level text
};

SpectrumFile parse_spectrum(const std::string& text) {
    SpectrumFile file;
    for (const std::string& line : lines_of(text)) {
        if (line.starts_with('#')) {
            file.comments.push_back(line);
        } else if (!line.empty()) {
            const std::size_t tab = line.find('\t');
            file.rows.emplace_back(line.substr(0, tab),
                                   tab == std::string::npos ? "" : line.substr(tab + 1));
        }
    }
    return file;
}

// The ENBW figure, which may differ by 0.0001 Hz, and the rest of the line,
// which may not.
struct WindowLine {
    std::string name;
    double enbw = 0.0;
};

std::optional<WindowLine> parse_window_line(const std::string& line) {
    constexpr std::string_view kPrefix = "# window: ";
    constexpr std::string_view kMiddle = ", ENBW ";
    constexpr std::string_view kSuffix = " Hz";
    if (!line.starts_with(kPrefix) || !line.ends_with(kSuffix)) {
        return std::nullopt;
    }
    const std::size_t middle = line.rfind(kMiddle);
    if (middle == std::string::npos) {
        return std::nullopt;
    }
    const std::string figure = line.substr(middle + kMiddle.size(),
                                           line.size() - kSuffix.size() - middle - kMiddle.size());
    const auto value = number(figure);
    if (!value) {
        return std::nullopt;
    }
    return WindowLine{line.substr(kPrefix.size(), middle - kPrefix.size()), *value};
}

// `slack_rows`: --min-db may land one row more or fewer at the threshold.
void expect_spectrum_matches(const std::string& what, const std::string& expected_text,
                             const std::string& actual_text, bool slack_rows, bool peak_row) {
    SCOPED_TRACE(what);
    const SpectrumFile expected = parse_spectrum(expected_text);
    const SpectrumFile actual = parse_spectrum(actual_text);

    ASSERT_EQ(actual.comments.size(), expected.comments.size()) << "comment line count";
    for (std::size_t i = 0; i < expected.comments.size(); ++i) {
        const auto want = parse_window_line(expected.comments[i]);
        const auto got = parse_window_line(actual.comments[i]);
        if (want && got) {
            EXPECT_EQ(got->name, want->name) << "window name";
            EXPECT_LE(std::abs(got->enbw - want->enbw), 0.0001 + kSlack)
                << "ENBW " << got->enbw << " vs " << want->enbw;
        } else {
            EXPECT_EQ(actual.comments[i], expected.comments[i]) << "comment line " << i;
        }
    }

    // Rows are matched by frequency text, which is exact, so a row more or
    // fewer at a --min-db threshold does not shift the comparison of the rest.
    if (slack_rows) {
        const auto difference = static_cast<std::ptrdiff_t>(actual.rows.size()) -
                                static_cast<std::ptrdiff_t>(expected.rows.size());
        EXPECT_LE(std::abs(difference), 1)
            << "row count " << actual.rows.size() << " vs " << expected.rows.size();
    } else {
        ASSERT_EQ(actual.rows.size(), expected.rows.size()) << "row count";
    }
    std::map<std::string, std::string> by_frequency;
    for (const auto& [frequency, level] : actual.rows) {
        by_frequency.emplace(frequency, level);
    }

    double worst = 0.0;
    std::size_t unmatched = 0;
    for (const auto& [frequency, level] : expected.rows) {
        const auto found = by_frequency.find(frequency);
        if (found == by_frequency.end()) {
            ++unmatched;
            continue;
        }
        const auto want = number(level);
        const auto got = number(found->second);
        ASSERT_TRUE(want.has_value() && got.has_value()) << "level column at " << frequency;
        const auto tolerance = peak_row ? std::optional<double>(0.01) : level_tolerance(*want);
        if (!tolerance) {
            continue;
        }
        const double difference = std::abs(*got - *want);
        EXPECT_LE(difference, *tolerance + kSlack)
            << "at " << frequency << " Hz: " << *got << " dB vs " << *want << " dB (allowed "
            << *tolerance << ")";
        worst = std::max(worst, difference);
    }
    if (slack_rows) {
        EXPECT_LE(unmatched, 1u) << "rows with no counterpart at the same frequency";
    } else {
        EXPECT_EQ(unmatched, 0u) << "frequency column differs";
    }
    testing::Test::RecordProperty("worst_level_difference_db", std::to_string(worst));
}

// ---------------------------------------------------------------- measure --

constexpr double kArrivalMs = 0.02;
constexpr double kReflectionMs = 0.02;
constexpr double kReflectionPercent = 2.0;
constexpr double kReverbSeconds = 0.02;
constexpr double kTableDb = 0.1;

bool is_number_with_unit(const std::vector<std::string>& words, std::size_t at,
                         std::string_view unit) {
    return at + 1 < words.size() && number(words[at]).has_value() && words[at + 1] == unit;
}

// The "agreement" line is checked for shape only: percentage, then a verdict in
// brackets. The value is a ratio of two decay fits and is not compared.
bool is_agreement_line(const std::string& line) {
    const auto words = words_of(line);
    if (words.size() < 4 || words[0] != "#" || words[1] != "agreement") {
        return false;
    }
    const std::string percent = strip_suffix(words[2], '%');
    return words[2].ends_with('%') && number(percent).has_value() && words[3].starts_with('(') &&
           line.ends_with(")");
}

// "#   EDT   0.141 s" or "#   EDT   (insufficient range)".
struct ReverbLine {
    std::string label;
    std::optional<double> seconds;  // nullopt: insufficient range
};

std::optional<ReverbLine> parse_reverb_line(const std::string& line) {
    const auto words = words_of(line);
    if (words.size() < 3 || words[0] != "#" ||
        (words[1] != "EDT" && words[1] != "T20" && words[1] != "T30")) {
        return std::nullopt;
    }
    if (line.ends_with("(insufficient range)")) {
        return ReverbLine{words[1], std::nullopt};
    }
    if (words.size() == 4 && words[3] == "s" && number(words[2])) {
        return ReverbLine{words[1], number(words[2])};
    }
    return std::nullopt;
}

void expect_measure_matches(const Row& row, const std::string& expected_text,
                            const std::string& actual_text) {
    const std::vector<std::string> expected = lines_of(expected_text);
    const std::vector<std::string> actual = lines_of(actual_text);
    ASSERT_EQ(actual.size(), expected.size()) << "line count";

    // The deconvolution of a signal against itself is a degenerate case whose
    // decay fits sit at about a millisecond, so there the reverberation lines
    // are checked for form only.
    const bool format_only_decay = row.id == "measure_files_identity";

    bool measured = false;
    for (std::size_t i = 0; i < expected.size(); ++i) {
        const std::string& want = expected[i];
        const std::string& got = actual[i];
        SCOPED_TRACE("line " + std::to_string(i + 1) + ": " + want);

        if (want.starts_with("# measured:")) {
            measured = true;
        }

        const auto want_words = words_of(want);
        const auto got_words = words_of(got);

        if (measured && want.find("direct arrival") != std::string::npos) {
            // "#   direct arrival     10.00 ms (3.43 m)"
            ASSERT_EQ(got_words.size(), want_words.size()) << got;
            ASSERT_GE(want_words.size(), 7u);
            const double want_ms = *number(want_words[3]);
            const double got_ms = number(got_words[3]).value_or(1e9);
            EXPECT_LE(std::abs(got_ms - want_ms), kArrivalMs + kSlack)
                << got_ms << " ms vs " << want_ms << " ms";
            // The distance is the time at 343 m/s, printed to two places.
            const double want_m = *number(strip_suffix(want_words[5].substr(1), ')'));
            const double got_m = number(got_words[5].substr(1)).value_or(1e9);
            EXPECT_LE(std::abs(got_m - want_m), kArrivalMs * 0.343 + 0.01 + kSlack)
                << got_m << " m vs " << want_m << " m";
        } else if (measured && want.find("impulse length") != std::string::npos) {
            EXPECT_EQ(got, want);
        } else if (measured && want.find("reflection") != std::string::npos) {
            // "#   reflection         +6.00 ms at 47%"
            ASSERT_EQ(got_words.size(), want_words.size()) << got;
            ASSERT_GE(want_words.size(), 6u);
            ASSERT_EQ(got_words[1], "reflection") << got;
            const double want_ms = *number(want_words[2]);
            const double got_ms = number(got_words[2]).value_or(1e9);
            EXPECT_LE(std::abs(got_ms - want_ms), kReflectionMs + kSlack)
                << got_ms << " ms vs " << want_ms << " ms";
            const double want_pct = *number(strip_suffix(want_words[5], '%'));
            const double got_pct = number(strip_suffix(got_words[5], '%')).value_or(1e9);
            EXPECT_LE(std::abs(got_pct - want_pct), kReflectionPercent + kSlack)
                << got_pct << "% vs " << want_pct << "%";
        } else if (const auto want_rt = parse_reverb_line(want)) {
            const auto got_rt = parse_reverb_line(got);
            ASSERT_TRUE(got_rt.has_value()) << "not a reverberation line: " << got;
            EXPECT_EQ(got_rt->label, want_rt->label);
            if (!format_only_decay) {
                ASSERT_EQ(got_rt->seconds.has_value(), want_rt->seconds.has_value())
                    << "(insufficient range) must match: " << got;
                if (want_rt->seconds) {
                    EXPECT_LE(std::abs(*got_rt->seconds - *want_rt->seconds),
                              kReverbSeconds + kSlack)
                        << *got_rt->seconds << " s vs " << *want_rt->seconds << " s";
                }
            }
        } else if (is_agreement_line(want)) {
            EXPECT_TRUE(is_agreement_line(got)) << "agreement line is malformed: " << got;
        } else if (!want.empty() && !want.starts_with('#')) {
            // A table row: frequency exact, level within 0.1 dB, flag exact.
            const std::vector<std::string> want_cols = split(want, '\t');
            const std::vector<std::string> got_cols = split(got, '\t');
            ASSERT_EQ(got_cols.size(), 3u) << got;
            ASSERT_EQ(want_cols.size(), 3u) << want;
            EXPECT_EQ(got_cols[0], want_cols[0]) << "frequency";
            const double want_db = *number(want_cols[1]);
            const double got_db = number(got_cols[1]).value_or(1e9);
            EXPECT_LE(std::abs(got_db - want_db), kTableDb + kSlack)
                << got_db << " dB vs " << want_db << " dB at " << want_cols[0] << " Hz";
            EXPECT_EQ(got_cols[2], want_cols[2]) << "trustworthy";
        } else {
            // Everything else is layout and labels: the constructed block, the
            // gated-response summary, the column header, the file names.
            EXPECT_EQ(got, want);
        }
    }
}

// ---------------------------------------------------------------- compare --

constexpr double kCompareDb = 0.0002;

// Layout exact; a number followed by "dB" within 0.0002; every other word
// exact, including the "at ... Hz" frequencies unless `ignore_frequencies`.
void expect_compare_matches(const std::string& what, const std::string& expected_text,
                            const std::string& actual_text, bool ignore_frequencies) {
    SCOPED_TRACE(what);
    const std::vector<std::string> expected = lines_of(expected_text);
    const std::vector<std::string> actual = lines_of(actual_text);
    ASSERT_EQ(actual.size(), expected.size()) << "line count";
    for (std::size_t i = 0; i < expected.size(); ++i) {
        SCOPED_TRACE("line " + std::to_string(i + 1) + ": " + expected[i]);
        const auto want = words_of(expected[i]);
        const auto got = words_of(actual[i]);
        ASSERT_EQ(got.size(), want.size()) << actual[i];
        for (std::size_t w = 0; w < want.size(); ++w) {
            if (is_number_with_unit(want, w, "dB") && got[w + 1] == "dB") {
                const double difference = std::abs(number(got[w]).value_or(1e9) - *number(want[w]));
                EXPECT_LE(difference, kCompareDb + kSlack)
                    << got[w] << " dB vs " << want[w] << " dB";
            } else if (ignore_frequencies && w > 0 && want[w - 1] == "at" && w + 1 < want.size() &&
                       want[w + 1] == "Hz") {
                // The location of a maximum that is a tie between neighbours.
            } else {
                EXPECT_EQ(got[w], want[w]) << "word " << w;
            }
        }
    }
}

// ------------------------------------------------------------------- test --

class GoldenTest : public testing::TestWithParam<Row> {};

TEST_P(GoldenTest, MatchesTheRustOutput) {
    const Row& row = GetParam();
    const Scratch scratch(row.id);

    // An artifact left over from the copy would let a command that wrote
    // nothing pass, so the run starts without it.
    if (row.artifact != "-") {
        fs::remove(scratch.path() / row.artifact);
    }

    const harness::Output result = harness::run_process(kBin, row.args, scratch.path());

    EXPECT_EQ(result.status, row.exit_status) << "exit status; stderr was:\n" << result.err;

    const std::string expected_stdout = read_file(kFixtures / row.stdout_file);
    const std::string expected_stderr =
        row.stderr_file == "-" ? std::string() : read_file(kFixtures / row.stderr_file);

    if (row.kind == "exact") {
        expect_exact("stdout", expected_stdout, result.out);
        expect_exact("stderr", expected_stderr, result.err);
    } else if (row.kind == "wavgen") {
        expect_exact("stdout", expected_stdout, result.out);
        expect_exact("stderr", expected_stderr, result.err);
        ASSERT_TRUE(fs::exists(scratch.path() / row.artifact))
            << row.artifact << " was not written";
        expect_wav_matches(read_file(kFixtures / row.artifact),
                           read_file(scratch.path() / row.artifact));
    } else if (row.kind == "spectrum") {
        const bool slack = row.has_arg("--min-db");
        const bool peak = row.has_arg("--peak");
        expect_exact("stderr", expected_stderr, result.err);
        if (row.artifact != "-") {
            // --out: the report goes to the file and stdout stays empty.
            expect_exact("stdout", expected_stdout, result.out);
            ASSERT_TRUE(fs::exists(scratch.path() / row.artifact))
                << row.artifact << " was not written";
            expect_spectrum_matches(row.artifact, read_file(kFixtures / row.artifact),
                                    read_file(scratch.path() / row.artifact), slack, peak);
        } else {
            expect_spectrum_matches("stdout", expected_stdout, result.out, slack, peak);
        }
        // An on-bin sine reads -6.0206 through every window: the scaling check.
        if (row.id.starts_with("sine_onbin_peak_")) {
            const SpectrumFile got = parse_spectrum(result.out);
            ASSERT_EQ(got.rows.size(), 1u);
            EXPECT_LE(std::abs(number(got.rows[0].second).value_or(1e9) + 6.0206), 0.01 + kSlack)
                << "level " << got.rows[0].second;
        }
    } else if (row.kind == "measure") {
        expect_exact("stderr", expected_stderr, result.err);
        expect_measure_matches(row, expected_stdout, result.out);
    } else if (row.kind == "compare") {
        const bool ignore = row.id == "compare_agree_pass" || row.id == "compare_agree_band";
        expect_compare_matches("stdout", expected_stdout, result.out, ignore);
        expect_compare_matches("stderr", expected_stderr, result.err, ignore);
    } else {
        FAIL() << "unknown comparison class '" << row.kind << "'";
    }
}

std::string case_name(const testing::TestParamInfo<Row>& info) {
    return info.param.id;
}

INSTANTIATE_TEST_SUITE_P(Manifest, GoldenTest, testing::ValuesIn(load_manifest()), case_name);

// A manifest that fails to load would instantiate no cases at all, and an empty
// suite passes. This is what notices.
TEST(GoldenManifest, ListsEveryRecordedInvocation) {
    const std::vector<Row> rows = load_manifest();
    EXPECT_EQ(rows.size(), 62u);
    for (const Row& row : rows) {
        EXPECT_TRUE(row.kind == "exact" || row.kind == "wavgen" || row.kind == "spectrum" ||
                    row.kind == "measure" || row.kind == "compare")
            << row.id << ": " << row.kind;
    }
}

// The callback block size is the audio driver's business, so it cannot change
// the answer. The fixtures say so; this says so of the C++ outputs, byte for
// byte, which is stronger than each being near its own fixture.
TEST(GoldenInvariants, BlockSizeDoesNotChangeTheSpectrum) {
    const Scratch scratch("block-invariant");
    const harness::Output small = harness::run_process(kBin, {"wav/pink_f32.wav"}, scratch.path());
    const harness::Output large =
        harness::run_process(kBin, {"--block", "1000", "wav/pink_f32.wav"}, scratch.path());
    ASSERT_TRUE(small.success()) << small.err;
    ASSERT_TRUE(large.success()) << large.err;
    EXPECT_EQ(large.out, small.out);

    // And the same holds of what the Rust recorded.
    EXPECT_EQ(read_file(kFixtures / "stdout/rta_pink_f32_block1000.txt"),
              read_file(kFixtures / "stdout/rta_pink_f32_default.txt"));
}

}  // namespace
}  // namespace analyzer::cli

#endif  // !defined(_WIN32)
