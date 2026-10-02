// The string behaviours the file formats lean on, each pinned to what the Rust
// standard library does with the same input.

#include "model/text.hpp"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

namespace analyzer::model::detail {
namespace {

std::string lossy(std::string_view text) {
    return utf8_lossy(std::as_bytes(std::span(text)));
}

using Lines = std::vector<std::string_view>;

TEST(Trim, StripsAsciiWhitespace) {
    EXPECT_EQ(trim("  a b \t\r\n"), "a b");
    EXPECT_EQ(trim(""), "");
    EXPECT_EQ(trim(" \t "), "");
    EXPECT_EQ(trim("x"), "x");
}

// str::trim strips Unicode White_Space, not just ASCII, so a hand-edited file
// with a non-breaking space around a value parses the same here.
TEST(Trim, StripsUnicodeWhiteSpace) {
    EXPECT_EQ(trim("\xC2\xA0"
                   "a"
                   "\xC2\xA0"),
              "a")
        << "U+00A0";
    EXPECT_EQ(trim("\xE3\x80\x80"
                   "a"),
              "a")
        << "U+3000";
    EXPECT_EQ(trim("\xE2\x80\x83"
                   "a"
                   "\xE2\x80\xA8"),
              "a")
        << "U+2003, U+2028";
    EXPECT_EQ(trim("\xC2\x85"
                   "a"),
              "a")
        << "U+0085";
}

TEST(Trim, LeavesOtherNonAsciiAlone) {
    EXPECT_EQ(trim("\xC3\xA9 "), "\xC3\xA9") << "e acute";
    // U+200B is a zero-width space, which is not White_Space.
    EXPECT_EQ(trim("\xE2\x80\x8B"
                   "a"),
              "\xE2\x80\x8B"
              "a");
}

TEST(Trim, StopsAtInvalidUtf8) {
    EXPECT_EQ(trim(" \xFF"
                   "a "),
              "\xFF"
              "a");
    // An overlong encoding of a space is not a space.
    EXPECT_EQ(trim("\xC1\xA0"
                   "a"),
              "\xC1\xA0"
              "a");
}

TEST(SplitOnce, SplitsAtTheFirstSeparator) {
    const auto split = split_once("key: a: b", ':');
    ASSERT_TRUE(split.has_value());
    EXPECT_EQ(split->first, "key");
    EXPECT_EQ(split->second, " a: b");
    EXPECT_FALSE(split_once("no separator", ':').has_value());
}

TEST(LinesOf, SplitsAtNewlines) {
    EXPECT_EQ(lines("a\nb\nc"), (Lines{"a", "b", "c"}));
    EXPECT_EQ(lines("a\nb\n"), (Lines{"a", "b"})) << "no empty final line";
    EXPECT_EQ(lines(""), Lines{});
    EXPECT_EQ(lines("\n"), (Lines{""}));
    EXPECT_EQ(lines("a\n\nb"), (Lines{"a", "", "b"}));
}

TEST(LinesOf, DropsACarriageReturnBeforeTheNewlineOnly) {
    EXPECT_EQ(lines("a\r\nb\r\n"), (Lines{"a", "b"}));
    EXPECT_EQ(lines("a\rb\n"), (Lines{"a\rb"})) << "a lone CR does not split";
    EXPECT_EQ(lines("a\r\nb\r"), (Lines{"a", "b\r"})) << "an unterminated last line keeps its CR";
}

TEST(Utf8Lossy, ValidTextPassesThrough) {
    EXPECT_EQ(lossy("plain"), "plain");
    EXPECT_EQ(lossy("caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5"),
              "caf\xC3\xA9 \xE2\x82\xAC \xF0\x9F\x8E\xB5");
    EXPECT_EQ(lossy(""), "");
}

TEST(Utf8Lossy, EachBadSequenceBecomesOneReplacement) {
    const std::string replacement = "\xEF\xBF\xBD";
    EXPECT_EQ(lossy("a\xFF"
                    "b"),
              "a" + replacement + "b");
    EXPECT_EQ(lossy("\x80\x80"), replacement + replacement) << "stray continuations";
    EXPECT_EQ(lossy("\xC0\xAF"), replacement + replacement) << "overlong lead and tail";
    // A three-byte sequence cut short is one replacement, and the byte that
    // broke it is decoded afresh.
    EXPECT_EQ(lossy("\xE2\x82"
                    "x"),
              replacement + "x");
    EXPECT_EQ(lossy("\xE2\x82"), replacement) << "truncated at the end";
    EXPECT_EQ(lossy("\xF0\x9F\x8E"), replacement);
    // Surrogates and values above U+10FFFF are refused: the lead is replaced and
    // each following byte, now a stray continuation, is replaced too.
    EXPECT_EQ(lossy("\xED\xA0\x80"), replacement + replacement + replacement);
    EXPECT_EQ(lossy("\xF4\x90\x80\x80"), replacement + replacement + replacement + replacement);
}

TEST(DebugQuote, QuotesAndEscapesLikeRust) {
    EXPECT_EQ(debug_quote("RIFF...."), "\"RIFF....\"");
    EXPECT_EQ(debug_quote(""), "\"\"");
    EXPECT_EQ(debug_quote("a\"b\\c"), "\"a\\\"b\\\\c\"");
    EXPECT_EQ(debug_quote("a\nb\rc\td"), "\"a\\nb\\rc\\td\"");
    EXPECT_EQ(debug_quote(std::string_view("a\0b", 3)), "\"a\\0b\"");
    EXPECT_EQ(debug_quote("\x01\x1B\x7F"), "\"\\u{1}\\u{1b}\\u{7f}\"");
    EXPECT_EQ(debug_quote("\xC2\x85"), "\"\\u{85}\"") << "C1 control";
    EXPECT_EQ(debug_quote("caf\xC3\xA9"), "\"caf\xC3\xA9\"") << "printable non-ASCII stays";
    EXPECT_EQ(debug_quote("it's"), "\"it's\"") << "a single quote is not escaped in a string";
}

}  // namespace
}  // namespace analyzer::model::detail
