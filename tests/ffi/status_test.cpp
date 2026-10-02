// Ported from crates/analyzer-ffi/src/lib.rs.

#include <stdexcept>
#include <string>

#include <gtest/gtest.h>

#include "ffi/internal.hpp"
#include "test_util.hpp"

namespace analyzer::ffi {
namespace {

using test::message_of;
using test::only_repeats_of;

TEST(Status, MessagesRoundTripAndTruncateSafely) {
    const AnalyzerStatus status = status_failure("device not found");
    EXPECT_EQ(status.code, 1);
    EXPECT_EQ(message_of(status), "device not found");

    // Over-long multi-byte input must not leave a broken buffer behind.
    std::string long_text;
    for (int i = 0; i < 500; ++i) {
        long_text += "\xC3\xA9";  // é
    }
    const AnalyzerStatus truncated = status_failure(long_text);
    const std::string text = message_of(truncated);
    EXPECT_LT(text.size(), static_cast<std::size_t>(ANALYZER_MESSAGE_LEN));
    EXPECT_TRUE(only_repeats_of(text, "\xC3\xA9")) << "truncated mid-character";
}

TEST(Status, ASuccessfulStatusIsEmpty) {
    const AnalyzerStatus status = status_ok();
    EXPECT_EQ(status.code, 0);
    EXPECT_EQ(status.message[0], 0);
}

// A null status pointer is legal everywhere, and must never be written through.
TEST(Status, SettingThroughANullPointerDoesNothing) {
    set_status(nullptr, status_failure("ignored"));
    AnalyzerStatus status = status_ok();
    set_status(&status, status_failure("kept"));
    EXPECT_EQ(status.code, 1);
    EXPECT_EQ(message_of(status), "kept");
}

// The guard is the one thing between a throw and the C caller.
TEST(Guard, AnExceptionBecomesTheFallback) {
    EXPECT_EQ(guard(7, []() -> int { throw std::runtime_error("boom"); }), 7);
    EXPECT_FALSE(guard(false, []() -> bool { throw 42; }));
    EXPECT_TRUE(guard(false, [] { return true; }));
    bool ran = false;
    guard([&] {
        ran = true;
        throw std::runtime_error("boom");
    });
    EXPECT_TRUE(ran);
}

TEST(Guard, AnExceptionIsReportedThroughTheStatus) {
    AnalyzerStatus status = status_ok();
    const bool result = guard_status(
        &status, false, []() -> bool { throw std::runtime_error("the device went away"); });
    EXPECT_FALSE(result);
    EXPECT_NE(status.code, 0);
    EXPECT_EQ(message_of(status), "the device went away");

    // And a null status is as legal here as everywhere else.
    EXPECT_FALSE(guard_status(nullptr, false, []() -> bool { throw 1; }));
}

TEST(Utf8, ValidatesAndReplaces) {
    EXPECT_TRUE(is_valid_utf8("plain ascii"));
    EXPECT_TRUE(is_valid_utf8("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5"));
    EXPECT_FALSE(is_valid_utf8("\xC3"));              // truncated
    EXPECT_FALSE(is_valid_utf8("\xC0\x80"));          // overlong
    EXPECT_FALSE(is_valid_utf8("\xED\xA0\x80"));      // surrogate
    EXPECT_FALSE(is_valid_utf8("\xF4\x90\x80\x80"));  // above U+10FFFF
    EXPECT_FALSE(is_valid_utf8("\xFF"));

    // One replacement character per invalid sequence.
    EXPECT_EQ(utf8_lossy("a\xFF"
                         "b"),
              "a\xEF\xBF\xBD"
              "b");
    EXPECT_EQ(utf8_lossy("\xE2\x82"), "\xEF\xBF\xBD");
    EXPECT_EQ(utf8_lossy("ok"), "ok");
}

TEST(Utf8, FloorStopsOnACharacterBoundary) {
    const std::string_view text = "a\xC3\xA9";  // a, é
    EXPECT_EQ(utf8_floor(text, 3), 3u);
    EXPECT_EQ(utf8_floor(text, 2), 1u) << "inside the two-byte character";
    EXPECT_EQ(utf8_floor(text, 1), 1u);
    EXPECT_EQ(utf8_floor(text, 99), 3u);
}

}  // namespace
}  // namespace analyzer::ffi
