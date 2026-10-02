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

}  // namespace
}  // namespace analyzer::ffi
