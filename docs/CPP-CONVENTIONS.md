# C++ conventions

How C++ is written in this core. Read this before adding or changing a module.
`src/dsp/window.*`, `src/dsp/fft.*` and `tests/dsp/{window,fft}_test.cpp` are the
worked example: when this document and your instinct disagree, match those
files. `docs/READING-ORDER.md` walks through the others.

The goal is idiomatic modern C++ that a C++ audio developer would recognise as
their own. The core began as a Rust codebase and was ported test for test (see
"The port from Rust" in `docs/HANDOFF.md`); what survived is the behaviour, the
tests and, above all, the comments explaining *why*. The syntax is C++.

## Layout

```
src/<module>/<concept>.hpp      declarations + the explanatory comments
src/<module>/<concept>.cpp      definitions
src/<module>/CMakeLists.txt     analyzer_module(<module> DEPS ...)
tests/<module>/<concept>_test.cpp
tests/<module>/CMakeLists.txt   analyzer_module_tests(<module>)
tests/support/                  shared test helpers (header-only)
third_party/                    vendored libraries; see its README
include/analyzer.h              the C ABI; not a C++ header, see "The C boundary"
```

One concept per file pair, and one test file per source file. A module's sources
are globbed, so a new file needs no CMake edit; `src/audio/` and `src/cli/` list
theirs explicitly because which files compile depends on the platform. A module
may include only the modules it links against (`src/CMakeLists.txt` documents
the order), and CMake enforces it: a missing dependency is a compile error.

Includes are by module path from `src/`: `#include "dsp/window.hpp"`. Order: the
file's own header first, then standard library, then third-party, then project
headers, each block alphabetical. Headers use `#pragma once`.

## Naming

| Thing | Style | Example |
|---|---|---|
| Namespaces | `analyzer::<module>` | `analyzer::dsp` |
| Types, enums, enumerators | `PascalCase` | `SpectrumAnalyzer`, `Shape::FlatTop` |
| Functions, methods, variables | `snake_case` | `coherent_gain()`, `bin_spacing_hz` |
| Private data members | trailing underscore | `samples_` |
| Constants | `k` prefix | `kFlatTop`, `kDrainFrames` |
| Macros (rare) | `ANALYZER_` prefix | `ANALYZER_EXPECTS` |
| Files | `snake_case.hpp/.cpp` | `transfer.hpp` |

Units stay in names: `delay_ms`, `level_db`, `hz`, `bin_spacing_hz`.

Implementation-only helpers go in an anonymous namespace in the `.cpp`. Things
that must be visible to a header but are not API go in `namespace detail`.

## Comments

The comments are the most valuable thing in the codebase. They say why the code
is the way it is, and a reader is expected to learn the domain from them.

- Every header opens with a comment saying what the module is for and the
  reasoning behind it.
- Every public type and function has a comment above its declaration in the
  header. State preconditions in prose ("`size` must be even"), and say what is
  thrown and when ("throws NothingToDoError if ...").
- Implementation comments stay with the code they explain, in the `.cpp`.
- Do not narrate the code (`// loop over bins`). Do comment wherever C++ does
  something the reader might not expect: a cast relying on layout, a `noexcept`
  that matters, a memory order on an atomic and why that one.
- Test comments record what each test guards against and which bug it came
  from. Keep them when editing a test. A few test files still open with
  "Ported from crates/..." - that is history, not a live reference.

## Coming from Rust, or elsewhere

Most of the codebase was designed in Rust, and a reader (or a contributor) who
knows either side may want the mapping. Where the C++ here picked a particular
idiom, this is it.

| Idea | C++ here |
|---|---|
| Borrowed slice, `&[f32]` / `&mut [f32]` | `std::span<const float>` / `std::span<float>`; never a pointer and a length |
| Owned buffer | `std::vector<T>`, sized once at construction on real-time paths |
| Complex float | `dsp::Complex32` (= `std::complex<float>`, `dsp/complex.hpp`) |
| Optional value, `Option<T>` | `std::optional<T>` |
| Fallible setup code, `Result<T, E>` | Return `T` and **throw** a module exception. See "Errors" |
| Assertion on a programmer error | `ANALYZER_EXPECTS(cond, "message")` from `base/contract.hpp`; aborts in every build |
| Runtime polymorphism, `Box<dyn Trait>` | Abstract class, virtual destructor, `std::unique_ptr<Base>` (`dsp::Fft`, `audio::AudioBackend`) |
| Trait used only for static dispatch | A template or a concept, or nothing |
| Default values | Default member initialisers; `= default` constructor |
| Plain enum | `enum class`, plus a `to_string` if tests or logs print it |
| Enum whose variants carry a little data | A struct with an `enum class` field and the data as members, built through named constructors (`WindowKind::tukey(0.25f)`) |
| Enum whose variants carry different data | `std::variant`, only when the struct form would leave most fields meaningless |
| `match` | `switch` on an `enum class` with no `default:`, so `-Wswitch` flags a missing case |
| Iterator chains | A plain `for` loop, or `<algorithm>` / `<numeric>` where clearer. Prefer the loop on real-time paths |
| `Arc<T>` | `std::shared_ptr<T>`; used for state shared by two ends, as in the ring |
| `Send` / `Sync` | No equivalent. The class comment says which thread owns what |
| Moving ownership to exactly one holder | Delete the copy operations, default the move ones (`CaptureSink`) |
| `#[derive(PartialEq)]` | `friend bool operator==(const T&, const T&) = default;` |
| Saturating `as` cast | `saturating_cast<To>(x)` from `base/numeric.hpp`. A bare `static_cast` from an out-of-range float is undefined behaviour |
| `#[should_panic]` | `EXPECT_DEATH(expr, "message")` in a suite named `<Name>DeathTest` |
| `cfg(target_os = ...)` | The macros in `audio/target.hpp` plus platform branches in CMake |

## Errors

Three kinds of failure, three mechanisms:

1. **Programmer error** - wrong buffer length, impossible size. `ANALYZER_EXPECTS`.
   It aborts with a message in every build, release included: `<cassert>` is
   compiled out by `NDEBUG`, and an error return would put a branch on every call
   of the real-time path for a condition that should never happen. Tests check
   the message with death tests, so keep the messages stable.
2. **Runtime failure during setup** - a file that does not parse, a device that
   has gone, a configuration the hardware refuses. Throw. Each module defines its
   exceptions in `<module>/error.hpp`, derived from `std::runtime_error`, with
   one class per case the caller might handle differently, and accessors for the
   structured pieces so a caller need not take a message apart.
3. **Anything on the audio or analysis thread** - never throws, never aborts on
   input data. Functions there are `noexcept`. A condition that can happen at
   runtime (an empty frame, silence) returns a value saying so: `std::optional`,
   a `bool`, a status enum.

The C ABI catches every exception at the boundary (`guard` in
`src/ffi/internal.hpp`) and turns it into an `AnalyzerStatus` or the call's
fallback value. Nothing unwinds into C or Swift.

## The C boundary

`include/analyzer.h` is plain C, written for Swift's importer, and hand-maintained.
It is the ABI contract with the apps, so treat it like a published interface:
never reorder fields, renumber enumerators or change a type in passing, and
change both apps in the same pass as the header.

The implementation in `src/ffi/` is C++ and compiles against a C++ view of the
same header that CMake generates at configure time (C23 enum branch, `noexcept`
added to every declaration). A definition whose signature drifts from the header
is therefore a compile error. Every entry point is `extern "C"`, `noexcept`, and
runs its body inside `guard`. Null pointers are tolerated and become a failure
return.

## Real-time code

Everything in `CLAUDE.md` under "Rules that are not style preferences" holds, and
C++ gives fewer guarantees than most languages, so be explicit:

- Allocate at construction or configuration, never in a function the analysis
  or audio thread calls per block. Size buffers once; take `std::span`s into
  them.
- Mark per-block functions `noexcept`.
- No `std::function`, `std::string` building, iostreams, locks or logging on
  those paths.
- Atomics take an explicit memory order and a comment saying why that one. The
  ring and the triple buffer are the examples; both are covered by tests that
  pass under ThreadSanitizer.
- The allocation trap aborts if an `rt_section()` allocates. Test binaries for
  `engine`, `audio`, `ffi` and `cli` link it, so a callback driven by one of
  those tests is held to the rule. Keep it passing; do not reach for
  `permit_alloc` to make it pass.

## Numerics

- DSP is single precision. Write float literals with `f` (`0.5f`); the build has
  `-Wdouble-promotion` and `-Wfloat-conversion` on to catch the rest.
- Where code deliberately computes in `double` - signal generation, stored
  measurements - keep the comment saying why.
- Use the `float` overloads: `std::sin(float)`, `std::abs(Complex32)`. Never the
  C `sinf` family or the `double` versions by accident.
- `-ffp-contract=off` is set for the whole build. Do not rely on, or turn on,
  fused multiply-add: the same core must produce the same numbers on every
  platform, because golden fixtures compare them.
- Numbers go to and from text through `base/number_text.hpp`, not printf or
  iostreams, so files are locale-independent and byte-stable. Parsing floats uses
  `strtod` rather than `std::from_chars`, which Apple's libc++ does not provide
  at this project's macOS deployment target.
- Float to integer conversion goes through `saturating_cast`.
- **Do not loosen a test tolerance to make a test pass** without first
  understanding why it differs, and say so in a comment if you do.

## Tests

- GoogleTest, one executable per module, run through ctest. Test names are
  `PascalCase` sentences describing the property
  (`BinCentredSineRecoversItsAmplitude`); the suite is the type or concept under
  test.
- `EXPECT_*` by default; `ASSERT_*` inside loops so one failure does not print
  four thousand.
- Death tests check contracts. The build runs them in the "threadsafe" style
  (re-execute the binary), which is slower and immune to threads a test left
  running.
- Feed tests a continuous stream, never a repeated block - see the trap in
  `CLAUDE.md`.
- Shared helpers live in `tests/support/` (signals, generated signals,
  spectra). Check what is there before writing your own.
- Golden fixtures (`tests/golden/`) pin CLI output and file formats. Read its
  README before changing a number that is printed.

## Formatting and warnings

`.clang-format` decides layout (Google-based, four-space indent, 100 columns);
run `clang-format -i` on every `.hpp`, `.cpp` and `.mm` you touch, and **never on
a CMake file** - it will rewrite it as C++. The build uses `-Wall -Wextra
-Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
-Wdouble-promotion -Wfloat-conversion -Wimplicit-fallthrough -Wcast-align` as
errors (`/W4 /WX` on MSVC, where the set is not yet held to zero in CI). Fix
warnings; do not silence them. No C-style casts. Third-party code is built
without these warnings.

## Building and testing

```bash
cmake -S . -B build/dev            # once
cmake --build build/dev -j8
ctest --test-dir build/dev -j8 --output-on-failure

# What ./check.sh also runs: AddressSanitizer + UBSan
cmake -S . -B build/asan -DANALYZER_SANITIZE=address
cmake --build build/asan -j8 && ctest --test-dir build/asan -j8

# What CI also runs on Linux: ThreadSanitizer, and Release
cmake -S . -B build/tsan -DANALYZER_SANITIZE=thread
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release
```

Debug builds also switch on the standard library's own bounds checks, so an
out-of-range `operator[]` aborts in a test instead of reading garbage.

A change is done when it builds without warnings under GCC and Clang, every test
passes in the plain and the ASan build, and clang-format reports nothing -
which is exactly `./check.sh`.

## Dependencies

Only what is in `third_party/`. Do not add a library, fetch anything at build
time, or use platform APIs outside `src/audio/`. A vendored library is used
behind one wrapper (KissFFT behind `dsp::Fft`, dr_wav behind `model/wav`) so it
can be swapped without touching the rest of the core.
