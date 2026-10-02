#include "base/os_error.hpp"

#include <gtest/gtest.h>

#include <cerrno>
#include <string>

namespace analyzer {
namespace {

// The number rides along with the message, so a report is searchable even
// where the C library's wording differs from one platform to the next.
TEST(OsErrorText, NamesTheMessageAndTheNumber) {
    const std::string text = os_error_text(ENOENT);
    EXPECT_NE(text.find("(os error " + std::to_string(ENOENT) + ")"), std::string::npos) << text;
    EXPECT_NE(text.rfind(" (os error", 0), 0u) << "the message comes first";
}

}  // namespace
}  // namespace analyzer
