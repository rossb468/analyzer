#!/usr/bin/env bash
#
# The pre-commit gate: format, lint, test. Exits non-zero if any stage fails.
#
# This exists because hand-rolling the three commands in a shell one-liner kept
# masking failures - a `| head` or a `|| true` swallows cargo's exit code and
# reports success from partial output. Three things committed broken that way
# before this script existed.

set -euo pipefail
cd "$(dirname "$0")"
export PATH="/opt/homebrew/opt/rustup/bin:$HOME/.cargo/bin:$PATH"

echo "==> fmt"
cargo fmt --all

echo "==> clippy"
cargo clippy --all-targets --all-features -- -D warnings

echo "==> test"
cargo test --workspace --all-features 2>&1 | tee /tmp/analyzer-test.log | grep -E "^(test result|error|warning: unused)" || true
# tee hides cargo's status, so take it from PIPESTATUS rather than the pipeline.
if [[ "${PIPESTATUS[0]}" -ne 0 ]]; then
    echo "TESTS FAILED" >&2
    grep -E "^---- |panicked" /tmp/analyzer-test.log | head -20 >&2
    exit 1
fi

total=$(grep -E "^test result" /tmp/analyzer-test.log | awk -F'[ ;]' '{t+=$4} END {print t}')
echo "==> ok: $total tests passing"
