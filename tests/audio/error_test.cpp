// Not ported from the Rust, which had no tests for error.rs. The messages are
// what a user reads when a device will not open, and the hierarchy is what a
// caller catches, so both are pinned here.

#include "audio/error.hpp"

#include <string>

#include <gtest/gtest.h>

namespace analyzer::audio {
namespace {

// The messages are the Rust Display strings, word for word.
TEST(AudioError, MessagesMatchTheRustDisplayText) {
    EXPECT_STREQ(NoBackendError().what(), "no audio backend is available on this platform");
    EXPECT_STREQ(DeviceNotFoundError("abc").what(), "audio device not found: abc");
    EXPECT_STREQ(UnsupportedSampleRateError("Mic", 48000.0).what(),
                 "Mic does not support 48000 Hz");
    EXPECT_STREQ(UnsupportedSampleRateError("Mic", 44100.5).what(),
                 "Mic does not support 44100.5 Hz");
    EXPECT_STREQ(ChannelOutOfRangeError("Mic", 7, 2).what(),
                 "channel 7 out of range for Mic, which has 2");
    EXPECT_STREQ(NothingToDoError().what(),
                 "stream configuration selects no input and no output channels");
    EXPECT_STREQ(AlreadyRunningError().what(), "stream is already running");
    EXPECT_STREQ(BackendError("boom").what(), "audio backend error: boom");
}

TEST(AudioError, EveryCaseCanBeCaughtAsTheCommonBase) {
    EXPECT_THROW(throw NoBackendError(), AudioError);
    EXPECT_THROW(throw DeviceNotFoundError("x"), AudioError);
    EXPECT_THROW(throw UnsupportedSampleRateError("x", 1.0), AudioError);
    EXPECT_THROW(throw ChannelOutOfRangeError("x", 1, 0), AudioError);
    EXPECT_THROW(throw NothingToDoError(), AudioError);
    EXPECT_THROW(throw AlreadyRunningError(), AudioError);
    EXPECT_THROW(throw BackendError("x"), AudioError);
    EXPECT_THROW(throw NoBackendError(), std::runtime_error);
}

TEST(AudioError, DerivedCasesCarryTheirData) {
    const ChannelOutOfRangeError error("Mic", 7, 2);
    EXPECT_EQ(error.device(), "Mic");
    EXPECT_EQ(error.channel(), 7u);
    EXPECT_EQ(error.available(), 2u);
    EXPECT_EQ(UnsupportedSampleRateError("Mic", 96000.0).requested(), 96000.0);
    EXPECT_EQ(DeviceNotFoundError("abc").device(), "abc");
}

}  // namespace
}  // namespace analyzer::audio
