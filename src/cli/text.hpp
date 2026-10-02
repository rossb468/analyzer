// Text helpers that only the harness needs.
//
// Number formatting and parsing are shared with the rest of the core, in
// base/number_text.hpp. What is left here has no use outside the command line:
// the Rust harness printed a few header lines with `{:?}`, and the golden files
// record that spelling.

#pragma once

#include <concepts>
#include <string>

namespace analyzer::cli {

// Rust `{:?}` for a float: the shortest decimal that reads back, as
// text::shortest() prints it, with `.0` appended to a whole number so that
// 1.0 stays `1.0` rather than `1`. NaN and the infinities print as in `{}`.
template <std::floating_point Float>
std::string debug_float(Float value);

}  // namespace analyzer::cli
