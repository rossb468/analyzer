# C++ conventions

How the C++ core is written, and how the port from Rust is done. Read this
before adding or porting a module. `src/dsp/window.*`, `src/dsp/fft.*` and
`tests/dsp/{window,fft}_test.cpp` are the worked example: when this document and
your instinct disagree, match those files.

## Goal of the port

Idiomatic modern C++ that a C++ audio developer would recognise as their own.
**Port the behaviour and the tests, not the syntax.** The Rust is the
specification - what each function promises, what each test checks, and above
all the comments explaining *why* - not a template to transliterate. Where
native C++ would structure something differently and better, do that, as long
as every ported test still passes.

## Layout

```
src/<module>/<concept>.hpp      declarations + the explanatory comments
src/<module>/<concept>.cpp      definitions
src/<module>/CMakeLists.txt     one line: analyzer_module(<module> DEPS ...)
tests/<module>/<concept>_test.cpp
tests/<module>/CMakeLists.txt   one line: analyzer_module_tests(<module>)
tests/support/                  shared test helpers (header-only)
third_party/                    vendored libraries; see its README
```

One Rust file `crates/analyzer-<module>/src/<concept>.rs` becomes
`src/<module>/<concept>.hpp` + `.cpp` and its `#[cfg(test)] mod tests` becomes
`tests/<module>/<concept>_test.cpp`. Integration tests under a crate's `tests/`
directory go in `tests/<module>/` too. Sources are globbed, so a new file needs
no CMake edit.

Includes are by module path from `src/`: `#include "dsp/window.hpp"`. Order:
the file's own header first, then standard library, then third-party, then
project headers, each block alphabetical.

## Naming

| Thing | Style | Example |
|---|---|---|
| Namespaces | `analyzer::<module>` | `analyzer::dsp` |
| Types, enums, enumerators | `PascalCase` | `SpectrumAnalyzer`, `Shape::FlatTop` |
| Functions, methods, variables | `snake_case` | `coherent_gain()`, `bin_spacing_hz` |
| Private data members | trailing underscore | `samples_` |
| Constants | `k` prefix | `kFlatTop`, `kMaxBufferFrames` |
| Macros (rare) | `ANALYZER_` prefix | `ANALYZER_EXPECTS` |
| Files | `snake_case.hpp/.cpp` | `transfer.hpp` |

Units stay in names as the Rust has them: `delay_ms`, `level_db`, `hz`.

## Comments

The Rust comments are the most valuable thing being ported. They say why the
code is the way it is, and a reader of this project is expected to learn the
domain from them.

- Every header opens with a comment saying what the module is for and the
  reasoning behind it - carried over from the Rust `//!` module comment.
- Every public type and function keeps its `///` comment as `//` above the
  declaration in the header. Drop Rust-only phrasing: `[`Foo`]` links become
  plain names, `# Panics` becomes "must be ..." in prose, `# Errors` becomes
  "throws X when ...".
- Implementation comments in the Rust stay with the code they explain in the
  `.cpp`.
- Do not add comments that narrate the code (`// loop over bins`). Do add one
  wherever C++ does something the reader might not expect (a cast relying on
  layout, a `noexcept` that matters, an ordering on an atomic).
- Test comments are carried over verbatim: they record what each test guards
  against and which bug it came from.

## Rust idiom to C++ idiom

| Rust | C++ |
|---|---|
| `&[f32]`, `&mut [f32]` | `std::span<const float>`, `std::span<float>` |
| `Vec<T>` | `std::vector<T>` |
| `Complex32` | `dsp::Complex32` (= `std::complex<float>`, see `dsp/complex.hpp`) |
| `Option<T>` | `std::optional<T>` |
| `Result<T, E>` from setup code (parsing, I/O, device open) | Return `T`, **throw** a module exception derived from `std::runtime_error`. See "Errors". |
| `assert!` / panic on a programmer error | `ANALYZER_EXPECTS(cond, "message")` from `base/contract.hpp` - aborts in every build |
| `#[should_panic(expected = "...")]` | `EXPECT_DEATH(expr, "...")` in a test suite named `<Name>DeathTest` |
| `trait` used for runtime polymorphism (`Box<dyn Fft>`) | Abstract class, virtual destructor, `std::unique_ptr<Base>` |
| `trait` used only for static dispatch or to satisfy the compiler | A template, a concept, or nothing - whichever reads best |
| `impl Default` | Default member initialisers, `= default` constructor |
| Plain C-like `enum` | `enum class` plus a `to_string` if tests or logs print it |
| `enum` whose variants carry a little data (`Tukey { alpha }`) | A struct with an `enum class` field and the data as members, built through named constructors (see `WindowKind`) |
| `enum` whose variants carry genuinely different data | `std::variant` - only when the struct form would leave most fields meaningless |
| `match` | `switch` on `enum class` (no `default:` - let `-Wswitch` catch a missing case), or `std::visit` |
| Iterator chains (`.iter().map().sum()`) | A plain `for` loop, or `<algorithm>` / `<numeric>` / ranges where that is clearer. Prefer the loop on real-time paths. |
| `Arc<T>` | `std::shared_ptr<T>` |
| `Mutex`, atomics | `std::mutex`, `std::atomic` with an explicit memory order and a comment saying why |
| `Send` / `Sync` | No equivalent. Say in the class comment which thread owns it. |
| `impl fmt::Debug` | Usually nothing. Add `to_string` / `operator<<` only where a test or log needs it. |
| `#[derive(PartialEq)]` | `friend bool operator==(const T&, const T&) = default;` |
| `pub(crate)` helper | Anonymous namespace in the `.cpp` |
| `f32::consts::PI`, `TAU` | `std::numbers::pi_v<float>`, `2.0f * pi` |
| `x as f32`, `x as usize` | `static_cast<float>(x)`, `static_cast<std::size_t>(x)` |
| `f32::total_cmp`, `.max_by` | `std::max_element` with a comparator |
| `u32`, `u64`, `i32`, `usize` | `std::uint32_t`, `std::uint64_t`, `std::int32_t`, `std::size_t` |

## Errors

Three kinds of failure, three mechanisms:

1. **Programmer error** - wrong buffer length, impossible size. `ANALYZER_EXPECTS`.
   Aborts with a message in every build. Tests check the message with death
   tests.
2. **Runtime failure during setup** - a file that does not parse, a device that
   has gone, a configuration the hardware refuses. Throw. Each module defines its
   exceptions in `<module>/error.hpp`, derived from `std::runtime_error`, with
   one class per case the caller might handle differently (the Rust error enum's
   variants). Messages carry the same information the Rust `Display` did.
3. **Anything on the audio or analysis thread** - never throws, never aborts on
   input data. Functions there are `noexcept`. A condition that can happen at
   runtime (an empty frame, silence) returns a value saying so: `std::optional`,
   a `bool`, a status enum.

The C ABI catches every exception at the boundary and turns it into an
`AnalyzerStatus`, exactly as the Rust caught panics. Nothing unwinds into C or
Swift.

## Real-time code

Everything in CLAUDE.md under "Rules that are not style preferences" still
holds, and C++ gives fewer guarantees, so be explicit:

- Allocate at construction or configuration, never in a function the analysis
  or audio thread calls per block. Size buffers once; take `std::span`s into
  them.
- Mark per-block functions `noexcept`.
- No `std::function`, `std::string` building, iostreams, locks or logging on
  those paths.
- The engine's allocation trap (debug and test builds) aborts if the audio
  callback allocates. Keep it passing.

## Numerics

- DSP is single precision. Write float literals with `f` (`0.5f`); the build
  has `-Wdouble-promotion` and `-Wfloat-conversion` on to catch the rest.
- Where the Rust deliberately computes in `f64`, do the same and keep its
  comment saying why.
- Use the `float` overloads: `std::sin(float)`, `std::abs(Complex32)`. Never the
  C `sinf` family or the `double` versions by accident.
- Expect results to match the Rust to within the test tolerances, not
  bit-for-bit - the FFT library differs. **Do not loosen a tolerance to make a
  test pass** without first understanding why it differs, and say so in a
  comment if you do.

## Tests

- Port every Rust test. Same name in `PascalCase` (`bin_centred_sine_recovers_its_amplitude`
  → `BinCentredSineRecoversItsAmplitude`), same assertions, same tolerances,
  same comments. Suite name is the type or concept under test.
- `EXPECT_*` by default; `ASSERT_*` inside loops so one failure does not print
  four thousand.
- Shared helpers go in `tests/support/`. Check what is there before writing
  your own.

## Formatting and warnings

`.clang-format` decides layout; run `clang-format -i` on every file you touch.
The build uses `-Wall -Wextra -Wpedantic -Wshadow -Wold-style-cast
-Wdouble-promotion -Wfloat-conversion` and more, as errors. Fix warnings; do not
silence them. No C-style casts.

## Building and testing

```bash
cmake -S . -B build/cpp            # once
cmake --build build/cpp -j8
ctest --test-dir build/cpp --output-on-failure

# What CI and ./check.sh also run: AddressSanitizer + UBSan
cmake -S . -B build/asan -DANALYZER_SANITIZE=address
cmake --build build/asan -j8 && ctest --test-dir build/asan
```

A module is done when it builds without warnings under GCC and Clang, every
ported test passes in both the plain and the ASan build, and clang-format
reports nothing.

## Dependencies

Only what is in `third_party/`. Do not add a library, fetch anything at build
time, or use platform APIs outside `src/audio/`.
