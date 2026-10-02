// The operating system's words for a failed call.
//
// The harness and the C ABI both report file errors in the form the apps
// already show to a person: `No such file or directory (os error
// 2)`. The errno number rides along because the message alone is not enough to
// search for.

#pragma once

#include <string>
#include <system_error>

namespace analyzer {

// The message for an errno value, with the number appended.
inline std::string os_error_text(int code) {
    return std::error_code(code, std::generic_category()).message() + " (os error " +
           std::to_string(code) + ")";
}

}  // namespace analyzer
