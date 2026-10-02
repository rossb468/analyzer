// Precondition checks that stay on in every build.
//
// A broken precondition here is a programmer error: a buffer of the wrong
// length, an FFT size of zero. There is no sensible way to carry on, and
// absorbing it into an error return would put a branch on every call of the
// real-time path for a condition that should never happen. So it aborts, with
// a message, in release as well as debug - the same behaviour the Rust core
// had with assert!, and the reason the check is a macro of our own rather
// than <cassert>, which NDEBUG compiles away.
//
// Runtime conditions that can legitimately happen - a file that does not
// parse, a device that has gone - are not contract violations. Those throw
// (setup code) or return a status (real-time code). See docs/CPP-CONVENTIONS.md.

#pragma once

#include <cstdio>
#include <cstdlib>

namespace analyzer {

// Report a violated precondition and abort. Never returns.
//
// noexcept and abort rather than throw: a contract violation inside the audio
// callback must not unwind through the platform's C code.
[[noreturn]] inline void contract_violation(const char* condition, const char* message,
                                            const char* file, int line) noexcept {
    std::fprintf(stderr, "%s:%d: contract violated: %s (%s)\n", file, line, message, condition);
    std::fflush(stderr);
    std::abort();
}

}  // namespace analyzer

// Check a precondition. `message` is a string literal saying what the caller
// got wrong; tests match on it, so keep it stable.
//
//   ANALYZER_EXPECTS(input.size() == size(), "input length must equal the FFT size");
#define ANALYZER_EXPECTS(condition, message) \
    ((condition) ? static_cast<void>(0)      \
                 : ::analyzer::contract_violation(#condition, message, __FILE__, __LINE__))
