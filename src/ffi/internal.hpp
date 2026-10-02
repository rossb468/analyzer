// Stable C ABI surface consumed by the platform user interfaces.
//
// Shape of the boundary
//
// Two kinds of traffic cross here and they have opposite needs, so they use
// different mechanisms:
//
// - Control and state - starting, stopping, choosing a device, resizing the
//   plot. Low frequency, so plain functions over opaque handles and POD
//   structs.
// - Frame data - a reduced trace, a few thousand floats, at up to 120 fps.
//   This is copied into caller-owned memory.
//
// Copying is deliberate. An earlier design handed out a pointer into the
// engine's triple buffer to avoid the copy, which is premature optimisation at
// this scale - a trace is a few kilobytes and a memcpy costs microseconds - and
// it introduced a real hazard, because that buffer is only valid until the
// consumer's next read and an asynchronous Metal upload can race it into a torn
// read.
//
// Rules for every entry point
//
// - Null pointers are tolerated and become a failure return, never a crash.
// - Exceptions are caught. Unwinding into C or Swift is undefined behaviour,
//   and a UI thread should not die because analysis hit an edge case. Every
//   entry point is `extern "C"` and `noexcept`, and its body runs inside
//   `guard`, which turns whatever was thrown into the call's fallback value.
// - Nothing allocates memory the caller must free, except the explicit
//   `_destroy` / `_stop` functions.
//
// The declarations themselves are include/analyzer.h, which is hand-maintained
// and is the contract with the apps. This file is the private half: the
// guard, the status helpers and the string plumbing that the entry points in
// the other files in this directory share.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "base/os_error.hpp"
#include "base/units.hpp"
#include "base/utf8.hpp"
#include "ffi/c_api.hpp"

namespace analyzer::ffi {

// Most bands an equaliser can carry across the boundary. See the header.
inline constexpr std::size_t kMaxEqBands = ANALYZER_MAX_EQ_BANDS;

// Longest callback a session will process in one pass.
//
// Buffers are sized for this once, at start. A device that hands over more
// than this has the excess dropped, which costs a visible overrun; growing a
// buffer on the audio thread would instead cost an audible one.
inline constexpr std::size_t kMaxCallbackFrames = 16'384;

// ---------------------------------------------------------------------------
// The guard
// ---------------------------------------------------------------------------

// Run `body`, converting anything it throws into `fallback`.
//
// Unwinding across the C boundary is undefined behaviour. Swallowing the
// exception is justified because a throw here abandons the call and discards
// its result; nothing observes a half-mutated state afterwards. `R` is the
// entry point's return type, written out at the call so that `guard<bool>(...)`
// cannot silently become an int.
template <class R, class F>
R guard(R fallback, F&& body) noexcept {
    try {
        return static_cast<R>(std::forward<F>(body)());
    } catch (const std::exception&) {
        // Falls through to the fallback.
    } catch (...) {
        // Not derived from std::exception: still must not reach C.
    }
    return fallback;
}

// As above for an entry point that returns nothing.
template <class F>
void guard(F&& body) noexcept {
    try {
        std::forward<F>(body)();
    } catch (const std::exception&) {
    } catch (...) {
    }
}

// ---------------------------------------------------------------------------
// Status
// ---------------------------------------------------------------------------

// Success: code zero, empty message.
AnalyzerStatus status_ok() noexcept;

// Failure: code one, with `message` truncated on a character boundary so the
// buffer stays valid UTF-8.
AnalyzerStatus status_failure(std::string_view message) noexcept;

// Write a status through an optional out-pointer.
void set_status(AnalyzerStatus* out, const AnalyzerStatus& status) noexcept;

// Run `body` like `guard`, and also report a thrown exception through `status`.
//
// A bare guard() leaves the status untouched when the body throws, which a
// caller that had not initialised it would read as garbage. An exception that
// escapes the body here is reported with its message instead.
template <class R, class F>
R guard_status(AnalyzerStatus* status, R fallback, F&& body) noexcept {
    try {
        return static_cast<R>(std::forward<F>(body)());
    } catch (const std::exception& error) {
        set_status(status, status_failure(error.what()));
    } catch (...) {
        set_status(status, status_failure("unexpected internal error"));
    }
    return fallback;
}

// ---------------------------------------------------------------------------
// Text
// ---------------------------------------------------------------------------

// Copy a string into a fixed NUL-terminated buffer, truncating on a character
// boundary so the result stays valid UTF-8. The whole buffer is cleared first.
void write_c_string(std::span<char> dest, std::string_view text) noexcept;

// Read a fixed NUL-terminated buffer back into a string, replacing invalid
// UTF-8 rather than failing.
std::string read_c_string(std::span<const char> source);

// A C string argument as UTF-8, or nothing when it is not valid UTF-8. `text`
// must not be null.
std::optional<std::string> checked_utf8(const char* text);

// ---------------------------------------------------------------------------
// Files
// ---------------------------------------------------------------------------

// Write `bytes` to `path`, creating or truncating it. Returns the error text on
// failure.
std::optional<std::string> write_file(const std::string& path, std::string_view bytes);

// The result of reading a whole text file.
struct FileRead {
    std::string text;
    // Set when the read failed, with the text of the failure.
    std::optional<std::string> error;
    // The file does not exist. Not an error for something like settings, where
    // it means first launch.
    bool not_found = false;
};

// Read `path` as UTF-8 text.
FileRead read_text_file(const std::string& path);

// ---------------------------------------------------------------------------
// Numbers
// ---------------------------------------------------------------------------

// Linear amplitude for a level in dBFS, capped at full scale.
//
// A level above 0 dBFS cannot be produced and would only clip, so the ceiling
// is enforced here rather than trusted to the caller.
inline float amplitude_from_db(float level_db) noexcept {
    return db_to_amplitude(std::fmin(level_db, 0.0f));
}

// `value` limited to [low, high], passing NaN through. Throws when the bounds
// are inverted or NaN, which is the case a clamp cannot answer.
float clamp_float(float value, float low, float high);

}  // namespace analyzer::ffi
