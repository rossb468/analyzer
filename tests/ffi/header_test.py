#!/usr/bin/env python3
"""Check include/analyzer.h against the header cbindgen generates.

include/analyzer.h is hand-maintained now, and the apps compile Swift against
it. While the Rust crate still exists it is the reference: the hand-written
header must be declaration-for-declaration identical to the generated one -
same type names, enumerator values, struct field order and types, function
signatures, constants, includes and include guard.

Comments and whitespace are stripped from both and the remaining token streams
are compared. Newlines survive only after preprocessor lines, because
`#define X 24` and `#define X` followed by `24` are different programs.

Exit status: 0 identical, 1 different, 77 the generated header is unavailable
(no cargo, or the Rust crate has been removed), which ctest reports as skipped.

    header_test.py [--generated PATH] [--header PATH]
"""

import argparse
import difflib
import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SKIP = 77

TOKEN = re.compile(r"[A-Za-z_][A-Za-z0-9_]*|\d+\w*|\S")


def strip_comments(text: str) -> str:
    """Remove block and line comments, leaving string literals alone."""
    out = []
    i = 0
    n = len(text)
    while i < n:
        two = text[i : i + 2]
        if two == "/*":
            end = text.find("*/", i + 2)
            if end < 0:
                raise ValueError("unterminated block comment")
            out.append(" ")
            i = end + 2
        elif two == "//":
            end = text.find("\n", i)
            i = n if end < 0 else end
        elif text[i] == '"':
            end = i + 1
            while end < n and text[end] != '"':
                end += 2 if text[end] == "\\" else 1
            out.append(text[i : end + 1])
            i = end + 1
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def tokens(text: str) -> list[str]:
    result: list[str] = []
    for line in strip_comments(text).splitlines():
        line_tokens = TOKEN.findall(line)
        if not line_tokens:
            continue
        result.extend(line_tokens)
        if line_tokens[0] == "#":
            result.append("<newline>")
    return result


def find_generated(explicit: str | None) -> pathlib.Path | None:
    if explicit:
        path = pathlib.Path(explicit)
        return path if path.exists() else None

    path = ROOT / "crates" / "analyzer-ffi" / "include" / "analyzer.h"
    if path.exists():
        return path
    if not (ROOT / "crates" / "analyzer-ffi").is_dir():
        return None

    cargo = shutil.which("cargo") or str(pathlib.Path.home() / ".cargo" / "bin" / "cargo")
    if not pathlib.Path(cargo).exists():
        return None
    try:
        subprocess.run([cargo, "build", "-p", "analyzer-ffi"], cwd=ROOT, check=True)
    except (OSError, subprocess.CalledProcessError):
        return None
    return path if path.exists() else None


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    parser.add_argument("--generated", help="the cbindgen-generated header")
    parser.add_argument("--header", default=str(ROOT / "include" / "analyzer.h"))
    args = parser.parse_args()

    generated = find_generated(args.generated)
    if generated is None:
        print("no generated header to compare against; skipping")
        return SKIP

    ours = tokens(pathlib.Path(args.header).read_text())
    theirs = tokens(generated.read_text())

    if ours == theirs:
        print(f"identical: {len(ours)} tokens in {args.header} and {generated}")
        return 0

    # Show the difference one token per line, which diffs far better than the
    # file does when only a comment or a layout choice differs elsewhere.
    sys.stdout.writelines(
        difflib.unified_diff(
            [t + "\n" for t in theirs],
            [t + "\n" for t in ours],
            fromfile=str(generated),
            tofile=args.header,
            n=6,
        )
    )
    print("analyzer.h differs from the generated header", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
