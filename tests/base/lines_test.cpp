#include "base/lines.hpp"

#include <gtest/gtest.h>

#include <string>

namespace analyzer::text {
namespace {

TEST(AppendLine, AddsTheTextAndANewline) {
    std::string out = "first\n";
    append_line(out, "second");
    append_line(out, std::string("third"));
    EXPECT_EQ(out, "first\nsecond\nthird\n");
}

TEST(AppendLine, NoArgumentIsABlankLine) {
    std::string out = "a\n";
    append_line(out);
    append_line(out, "");
    EXPECT_EQ(out, "a\n\n\n");
}

}  // namespace
}  // namespace analyzer::text
