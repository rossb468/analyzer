#include "base/utf8.hpp"

#include <gtest/gtest.h>

#include <string_view>

namespace analyzer {
namespace {

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
}  // namespace analyzer
