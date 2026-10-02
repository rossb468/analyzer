#!/usr/bin/env bash
#
# The pre-commit gate: format, lint, test. Exits non-zero if any stage fails.
#
# This exists because hand-rolling the three commands kept masking failures - a
# `| head` or a `|| true` swallows cargo's exit code and reports success from
# partial output.
#
# The first version of this script had exactly that bug. `cargo test | tee |
# grep || true` resets PIPESTATUS from the `true`, so a failing suite reported
# success and a broken commit went through. Nothing is piped now: output goes to
# a file, the status is captured immediately, and the file is searched after.

set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"

LOG=/tmp/analyzer-test.log

echo "==> fmt"
cargo fmt --all

echo "==> clippy"
cargo clippy --all-targets --all-features -- -D warnings

# The iOS backend only compiles for iOS, so the host clippy above never sees
# it. Type-checking it needs the target's standard library and nothing else.
if rustup target list --installed 2>/dev/null | grep -x aarch64-apple-ios > /dev/null; then
    echo "==> clippy (iOS)"
    cargo clippy --all-targets --all-features --target aarch64-apple-ios \
        -p analyzer-audio -p analyzer-ffi -- -D warnings
else
    echo "==> clippy (iOS) skipped: rustup target add aarch64-apple-ios"
fi

echo "==> test"
set +e
cargo test --workspace --all-features > "$LOG" 2>&1
status=$?
set -e

if [[ $status -ne 0 ]]; then
    echo "TESTS FAILED (exit $status)" >&2
    grep -E "^---- " "$LOG" | head -20 >&2
    grep -A3 "panicked at" "$LOG" | head -40 >&2
    exit "$status"
fi

total=$(grep -E "^test result" "$LOG" | awk -F'[ ;]' '{t+=$4} END {print t}')
echo "==> ok: $total Rust tests passing"

# ---------------------------------------------------------------------------
# The C++ core. Runs beside the Rust one until the port is complete.
# ---------------------------------------------------------------------------

CPP_SOURCES=$(find src tests/support tests/dsp tests/cal tests/plot tests/model tests/engine \
    tests/audio tests/ffi tests/cli -name '*.hpp' -o -name '*.cpp' -o -name '*.h' -o -name '*.mm' \
    2> /dev/null || true)

echo "==> clang-format"
# shellcheck disable=SC2086 # one path per word
clang-format --dry-run --Werror $CPP_SOURCES

CPP_LOG=/tmp/analyzer-cpp-test.log
for flavour in plain address; do
    dir="build/check-$flavour"
    sanitize=""
    [[ "$flavour" == "address" ]] && sanitize="address"
    echo "==> C++ build ($flavour)"
    cmake -S . -B "$dir" -DANALYZER_SANITIZE="$sanitize" > /dev/null
    cmake --build "$dir" -j "$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)" > "$CPP_LOG" 2>&1 \
        || { grep -E "error|warning" "$CPP_LOG" | head -40 >&2; exit 1; }

    echo "==> C++ tests ($flavour)"
    set +e
    ctest --test-dir "$dir" --output-on-failure -j 4 > "$CPP_LOG" 2>&1
    status=$?
    set -e
    if [[ $status -ne 0 ]]; then
        echo "C++ TESTS FAILED ($flavour)" >&2
        grep -E "Failed|\*\*\*" "$CPP_LOG" | head -40 >&2
        exit "$status"
    fi
    grep -E "tests passed" "$CPP_LOG"
done
echo "==> ok"
