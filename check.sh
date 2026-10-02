#!/usr/bin/env bash
#
# The pre-commit gate: format, build with warnings as errors, test - plainly
# and under AddressSanitizer and UndefinedBehaviorSanitizer. Exits non-zero if
# any stage fails.
#
# This exists because hand-rolling the commands kept masking failures - a
# `| head` or a `|| true` swallows the exit code and reports success from
# partial output. The first version of this script had exactly that bug:
# `test | tee | grep || true` reset PIPESTATUS from the `true`, so a failing
# suite reported success and a broken commit went through. Nothing is piped
# now: output goes to a file, the status is captured immediately, and the file
# is searched after.

set -euo pipefail
cd "$(dirname "$0")"

LOG=/tmp/analyzer-check.log
JOBS="$(getconf _NPROCESSORS_ONLN 2> /dev/null || echo 4)"

echo "==> clang-format"
# Sources only. clang-format will happily "format" a CMakeLists.txt as C++.
find src tests -name '*.hpp' -o -name '*.cpp' -o -name '*.mm' \
    | xargs clang-format --dry-run --Werror

for flavour in plain address; do
    dir="build/check-$flavour"
    sanitize=""
    [[ "$flavour" == "address" ]] && sanitize="address"

    echo "==> build ($flavour)"
    cmake -S . -B "$dir" -DANALYZER_SANITIZE="$sanitize" > /dev/null
    set +e
    cmake --build "$dir" -j "$JOBS" > "$LOG" 2>&1
    status=$?
    set -e
    if [[ $status -ne 0 ]]; then
        echo "BUILD FAILED ($flavour)" >&2
        grep -E "error|warning" "$LOG" | head -40 >&2
        exit "$status"
    fi

    echo "==> test ($flavour)"
    set +e
    ctest --test-dir "$dir" --output-on-failure -j "$JOBS" > "$LOG" 2>&1
    status=$?
    set -e
    if [[ $status -ne 0 ]]; then
        echo "TESTS FAILED ($flavour)" >&2
        grep -E "Failed|\*\*\*" "$LOG" | head -40 >&2
        exit "$status"
    fi
    grep -E "tests passed" "$LOG"
done

echo "==> ok"
