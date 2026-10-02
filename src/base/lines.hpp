// Building line-oriented text.
//
// The exports, the reports and the harness's output are all assembled a line at
// a time into one std::string. Every one needs the same two-line helper, so it
// lives here once.

#pragma once

#include <string>
#include <string_view>

namespace analyzer::text {

// Append `line` and a newline to `out`.
inline void append_line(std::string& out, std::string_view line = {}) {
    out += line;
    out += '\n';
}

}  // namespace analyzer::text
