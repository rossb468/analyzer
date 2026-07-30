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
echo "==> ok: $total tests passing"
