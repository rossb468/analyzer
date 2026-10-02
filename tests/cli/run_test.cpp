#include "cli/run.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

#include "cli/args.hpp"

namespace analyzer::cli {
namespace {

struct Result {
    int status;
    std::string out;
    std::string err;
};

Result invoke(std::initializer_list<const char*> items) {
    const std::vector<std::string> argv(items.begin(), items.end());
    std::ostringstream out;
    std::ostringstream err;
    const int status = run(argv, out, err);
    return {status, out.str(), err.str()};
}

std::filesystem::path scratch(const std::string& name) {
    return std::filesystem::temp_directory_path() / ("analyzer-cli-run-test-" + name);
}

// The in-process entry point is what main() calls, so the exit status and the
// "analyzer-cli: " prefix are decided here.
TEST(Run, HelpPrintsTheUsageAndSucceeds) {
    const Result result = invoke({"--help"});
    EXPECT_EQ(result.status, 0);
    EXPECT_EQ(result.out, usage());
    EXPECT_TRUE(result.err.empty());
}

TEST(Run, AFailureIsOneLineOnStderrAndStatusOne) {
    const Result result = invoke({"--frobnicate"});
    EXPECT_EQ(result.status, 1);
    EXPECT_TRUE(result.out.empty());
    EXPECT_EQ(result.err, "analyzer-cli: unknown option '--frobnicate' (try --help)\n");
}

TEST(Run, ASineIsAnalysedOffline) {
    const Result result = invoke({"--sine", "996.09375", "--peak", "--window", "rect"});
    ASSERT_EQ(result.status, 0) << result.err;
    EXPECT_NE(result.out.find("# source: 48000 frames offline"), std::string::npos);
    EXPECT_NE(result.out.find("996.093750\t-6.0206"), std::string::npos);
}

TEST(Run, OutWritesTheReportToAFileAndSaysSoOnStderr) {
    const std::filesystem::path path = scratch("report.txt");
    std::filesystem::remove(path);
    const std::string file = path.string();

    const Result result = invoke({"--sine", "1000", "--peak", "--out", file.c_str()});
    ASSERT_EQ(result.status, 0) << result.err;
    EXPECT_TRUE(result.out.empty());
    EXPECT_EQ(result.err, "wrote " + file + "\n");

    std::string text;
    {
        // Closed before the remove below: Windows will not delete a file that
        // is still open.
        std::ifstream written(path);
        std::ostringstream buffer;
        buffer << written.rdbuf();
        text = buffer.str();
    }
    EXPECT_NE(text.find("# analyzer-cli spectrum"), std::string::npos);
    std::filesystem::remove(path);
}

// A WAV that is too short, and a channel that is not there, are refused before
// any audio is pushed.
TEST(Run, RefusesASourceThatCannotFillAFrameOrLacksTheChannel) {
    EXPECT_EQ(invoke({"--sine", "1000", "--seconds", "0.05"}).err,
              "analyzer-cli: need at least 4096 frames for a 4096-point FFT, source has 2400\n");
    EXPECT_EQ(invoke({"--sine", "1000", "--channel", "1"}).err,
              "analyzer-cli: channel 1 requested but the source has 1\n");
}

// A block bigger than the ring cannot be written without dropping audio, and a
// measurement across dropped audio is wrong, not merely noisy.
TEST(Run, DroppedBlocksFailTheRunInsteadOfBeingHidden) {
    const Result result = invoke({"--sine", "1000", "--block", "20000"});
    EXPECT_EQ(result.status, 1);
    EXPECT_NE(result.err.find("dropped - the measurement is invalid"), std::string::npos)
        << result.err;
}

TEST(Run, ToleranceMakesCompareAGate) {
    const std::filesystem::path a = scratch("a.txt");
    const std::filesystem::path b = scratch("b.txt");
    std::ofstream(a) << "100\t0.0\n1000\t0.0\n10000\t0.0\n";
    std::ofstream(b) << "100\t-1.0\n1000\t0.0\n10000\t1.0\n";
    const std::string first = a.string();
    const std::string second = b.string();

    EXPECT_EQ(invoke({"--compare", first.c_str(), second.c_str()}).status, 0);
    const Result gated = invoke({"--compare", first.c_str(), second.c_str(), "--tolerance", "0.1"});
    EXPECT_EQ(gated.status, 1);
    EXPECT_TRUE(gated.out.empty());
    EXPECT_NE(gated.err.find("shapes differ by 1.0000 dB at"), std::string::npos) << gated.err;
    EXPECT_NE(gated.err.find("tolerance 0.1000 dB"), std::string::npos) << gated.err;

    std::filesystem::remove(a);
    std::filesystem::remove(b);
}

}  // namespace
}  // namespace analyzer::cli
