// Text helpers shared by the file formats.
//
// Internal to this module; not part of its interface.
//
// The formats here were defined by what the Rust standard library does with a
// string - `str::trim`, `str::lines`, `String::from_utf8_lossy`, the Debug form
// of a string in an error message - and files written by one implementation have
// to be read by the other. These reproduce those behaviours rather than the
// nearest C++ idiom, because a hand-edited file with a non-breaking space in it
// must parse the same way in both.

#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace analyzer::model::detail {

// Strip leading and trailing Unicode White_Space, as `str::trim`.
//
// ASCII space and the control whitespace, plus U+0085, U+00A0, U+1680,
// U+2000..U+200A, U+2028, U+2029, U+202F, U+205F and U+3000. Text that is not
// valid UTF-8 simply stops the strip at the bad byte.
std::string_view trim(std::string_view text) noexcept;

// Split into lines, as `str::lines`: at "\n", dropping a "\r" that directly
// precedes it, with no empty final line after a trailing newline. A "\r" not
// followed by "\n" stays in the line.
std::vector<std::string_view> lines(std::string_view text);

// Split at the first `separator`, or nullopt if there is none.
std::optional<std::pair<std::string_view, std::string_view>> split_once(std::string_view text,
                                                                        char separator) noexcept;

// Decode bytes as UTF-8, replacing each maximal ill-formed subsequence with
// U+FFFD, as `String::from_utf8_lossy`.
std::string utf8_lossy(std::span<const std::byte> bytes);

// The text in double quotes with the escapes Rust's `{:?}` uses for a string
// (`\"`, `\\`, `\n`, `\r`, `\t`, `\0`, and `\u{..}` for other control
// characters), for error messages.
//
// Characters that are merely unprintable rather than control codes are not
// escaped; an error message does not need that much fidelity.
std::string debug_quote(std::string_view text);

}  // namespace analyzer::model::detail
