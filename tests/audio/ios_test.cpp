// Ported from crates/analyzer-audio/src/ios.rs.
//
// The iOS backend only exists on iOS, so the tests do too. Everywhere else the
// file is empty after the preprocessor, which keeps the test glob in CMake
// platform-agnostic.

#include "audio/target.hpp"

#if ANALYZER_AUDIO_IOS

#include "audio/ios.hpp"

#include <gtest/gtest.h>

namespace analyzer::audio {
namespace {

// The render callback hands its input buffer list to CoreAudio as an
// AudioBufferList. That is only sound while the header and the first
// buffer sit where the C struct puts them.
TEST(IosBackend, TheInputListIsLaidOutAsAnAudioBufferList) {
    EXPECT_TRUE(detail::input_list_matches_audio_buffer_list());
}

TEST(IosBackend, TheRemoteIoSubtypeIsRioc) {
    EXPECT_EQ(detail::remote_io_subtype(), 0x72696f63u);
}

}  // namespace
}  // namespace analyzer::audio

#endif  // ANALYZER_AUDIO_IOS
