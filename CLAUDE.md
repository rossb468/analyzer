# Working on `analyzer`

A native replacement for Room EQ Wizard. C++20 core, native UI per platform,
macOS first and deep before any other client starts.

`docs/HANDOFF.md` is the longer narrative: why the architecture is shaped this
way, what is built, what is not, and what to do next. Read it when you need the
reasoning rather than the rules. `docs/CPP-CONVENTIONS.md` is how C++ is written
here, and `docs/READING-ORDER.md` is a guided tour of the code for someone new
to it.

The original plan - milestones, performance targets, REW-compatibility
guardrails - lives at `~/.claude/plans/radiant-twirling-lerdorf.md`, outside the
repository. `docs/HANDOFF.md` carries everything from it that still matters.

## Environment

- **CMake 3.24+** and a **C++20 compiler**: GCC 13, Clang 18, AppleClang (Xcode)
  or MSVC. All four build the core; CI runs Linux (GCC), macOS (AppleClang) and
  Windows (MSVC).
- **clang-format** on the path. `./check.sh` fails on any formatting diff.
- No cargo, no Rust: the Rust core is gone. Its last revision is `e9a459b`, kept
  only so the golden fixtures can be regenerated (`tests/golden/README.md`).
- Xcode for the apps. Nothing is fetched at build time; everything the core
  depends on is vendored in `third_party/`.

## Commands

```bash
cmake -S . -B build/dev                  # once; Debug unless you say otherwise
cmake --build build/dev -j8
ctest --test-dir build/dev -j8 --output-on-failure
```

| Task | Command |
|---|---|
| The gate - format, build with warnings as errors, test plain and under ASan+UBSan | `./check.sh` |
| Build / run one module's tests | `cmake --build build/dev --target analyzer_dsp_tests && build/dev/tests/dsp/analyzer_dsp_tests --gtest_filter='Window*'` |
| Run tests by name through ctest | `ctest --test-dir build/dev -R '^Window'` (names are `Suite.Test`, not module names) |
| Headless harness | `build/dev/src/cli/analyzer-cli --help` |
| Performance (needs a Release build) | `build/rel/src/cli/analyzer-cli --bench` |
| End-to-end sanity | `build/dev/src/cli/analyzer-cli --measure-demo` |
| Write a test signal | `build/dev/src/cli/analyzer-cli --generate pink --out pink.wav` |
| Compare two exports | `build/dev/src/cli/analyzer-cli --compare ours.txt theirs.txt` |
| Release build | `cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release && cmake --build build/rel -j8` |
| ThreadSanitizer (ring, triple buffer, engine) | `cmake -S . -B build/tsan -DANALYZER_SANITIZE=thread && cmake --build build/tsan -j8 && ctest --test-dir build/tsan` |
| The library the apps link (Apple only) | `cmake --build build/dev --target analyzer_bundle` -> `build/dev/lib/libanalyzer.a` |

With a multi-config generator (Visual Studio, Xcode) the CLI lands in
`build/dev/src/cli/<Config>/` and ctest needs `--build-config <Config>`.

`./check.sh` must be green before every commit. It is not decorative: three
broken commits shipped before it was fixed. It does not run Release or TSan;
CI does, on Linux.

## Layout

```
include/analyzer.h     the C ABI. Hand-maintained; the contract with the apps
src/base/              contract checks, saturating_cast, Rust-compatible number text
src/dsp/               the maths. No I/O, no OS deps. FFT, windows, spectra,
                       transfer function, filters, deconvolution, EQ, optimiser
src/cal/               calibration chain, dBFS to absolute dB SPL
src/plot/              display reduction + axis transforms. Emits no pixels
src/model/             measurements, versioned format, REW text export,
                       filter export, program settings, WAV I/O, comparison
src/engine/            RT graph, lock-free ring, triple buffer, allocation trap
src/audio/             AudioBackend, offline backend, CoreAudio (macOS) and
                       RemoteIO (iOS); the only module that may touch Apple SDKs
src/ffi/               the C ABI implementation; every entry point is noexcept
src/cli/               headless harness: WAV analysis, live capture, bench,
                       sweep measure, signal generation, comparison
tests/<module>/        GoogleTest, one <concept>_test.cpp per source file
tests/golden/          outputs recorded from the Rust build; the C++ is held to them
tests/support/         shared test helpers (header-only)
third_party/           KissFFT, dr_wav, GoogleTest, copied in. See its README
cmake/                 AnalyzerModule.cmake (flags, sanitizers, module helper),
                       AnalyzerBundle.cmake (the merged libanalyzer.a)
```

The dependency graph is acyclic and enforced by CMake: `base` is under
everything; `dsp`, `cal` and `plot` are leaves above it; `model` and `engine`
sit on `dsp`; `audio` stands alone; `ffi` sits on all of them; `cli` on most. A
module that includes something it does not link against fails to compile.
Includes are by module path from `src/`: `#include "dsp/window.hpp"`.

## Rules that are not style preferences

Violating any of these is a real bug.

**The audio callback does only: generate, write output, write into the ring,
return.** No allocation, no locks, no logging, no syscalls, no exceptions.
Everything is preallocated at device-open. The callback body runs inside
`engine::rt_section()` (`src/engine/rt.hpp`); the target `analyzer::alloc_trap`
(`src/engine/alloc_trap.cpp`) replaces the global `operator new`, and the
replacement aborts the process if it runs inside an `rt_section`. Test binaries
for `engine`, `audio`, `ffi` and `cli` link it, and so does the Debug
`analyzer-cli`; a Release build does not, so shipped code pays nothing and
`rt_section` is inert. The trap cannot see C's `malloc` - the only C library on
the audio path is KissFFT, which is built to use the stack. Mark per-block
functions `noexcept`.

**No application logic in the clients.** Analysis config, unit conversion,
smoothing, axis scaling, trace management, calibration and file I/O live here. A
client must never compute a bin-to-pixel mapping; it calls `analyzer_freq_to_x`,
`analyzer_x_to_freq`, `analyzer_db_to_y`, `analyzer_y_to_db`,
`analyzer_phase_to_y`, `analyzer_coherence_to_y`. This decides whether the
Windows and Linux ports are weeks or months, and the temptation to break it
peaks when moving fast.

The clients live in their own repositories
([analyzer-macos](https://github.com/rossb468/analyzer-macos) and
[analyzer-ios](https://github.com/rossb468/analyzer-ios)) and consume this one
as a pinned submodule. **`include/analyzer.h` is hand-maintained and is the ABI
contract with both.** Swift compiles against that exact text. Changing it - a
type, a field, an enumerator value, a signature - needs matching changes in both
apps in the same pass, and their CI is what catches the mismatch. Do not
reorder struct fields or renumber enumerators.

**No Apple SDK types anywhere but `src/audio/`.** Only that module may include
an Apple header, and only behind the platform guards in `audio/target.hpp`
(`ANALYZER_AUDIO_COREAUDIO`, `ANALYZER_AUDIO_IOS`) and the platform branches in
`src/audio/CMakeLists.txt`. Everything else - the FFI included - must build on
Linux and Windows, which CI checks on every push. Nothing outside `src/audio/`
names a backend type; it asks `audio::default_backend()`.

The headless harness follows the same rule: live capture is confined to
`live_coreaudio.cpp`, compiled only on macOS (`live_unsupported.cpp` elsewhere),
so WAV analysis, the bench and swept measurement run anywhere.

**Storage is unsmoothed, complex, at native sample rate.** Smoothing and
fractional-octave banding are view transforms. Storing smoothed magnitude
forecloses group delay, RT60, minimum-phase decomposition and REW interop. An
unmeasured SPL offset stays `std::optional` (empty) rather than becoming zero -
"not measured" and "measured as needing no correction" are different facts.

**The core emits no pixels.** Line traces are CPU-side geometry in `src/plot/`.
The waterfall is one column of data per frame into a GPU ring texture.
Compositing Retina-resolution pixels on the CPU is roughly what REW does and is
why its waterfall is slow.

**Dropped blocks are counted and surfaced, never hidden.** A measurement taken
across dropped audio is wrong, not merely noisy.

**Three kinds of failure, three mechanisms.** A broken precondition (wrong
buffer length) is `ANALYZER_EXPECTS` from `base/contract.hpp`: it aborts with a
message in every build, release included. A runtime failure during setup (a file
that does not parse, a device that has gone) throws a module exception derived
from `std::runtime_error`. Nothing on the audio or analysis thread throws or
aborts on input data; it returns a value saying so. The C ABI catches every
exception at the boundary, so nothing unwinds into C or Swift.

## Git

Conventional Commits, scope is the module name (`dsp`, `engine`, `ffi`, ...).
Short-lived feature branches (`feat/`, `fix/`, `perf/`, `refactor/`, `chore/`),
rebase or squash onto `main`, no PRs needed. Every commit must build and pass
tests. History is intended to be made public.

## Traps this codebase has already hit

Each of these cost real time. Most now have a guard or a regression test.

- **Never pipe a test command.** `ctest | tee | grep || true` resets
  `PIPESTATUS` and reports success from a failing suite. `check.sh` redirects to
  a file and captures the status directly.
- **`kDrainFrames` in `engine.cpp` is load-bearing, not a tuning knob.** Draining
  the ring until empty lets a fast producer starve publication - 7332 frames
  analysed, 0 published. Test: `Engine.AFastProducerCannotStarvePublication`.
- **Feed tests a continuous stream, never a repeated block.** A repeated block
  is periodic, and that periodicity is never what the test is measuring. It has
  bitten twice: the delay finder correctly reported -3968 for a 128-sample delay,
  because cross-correlation cannot tell `d` from `d` minus the period; and the
  engine's average-reset test re-sent 5.3125 cycles, whose phase discontinuity
  smeared the tone off its bin and made the assertion depend on timing. Wrap a
  rolling offset through a buffer holding a whole number of cycles.
- **Measure filter response by RMS, never peak.** Sampling a sine rarely lands
  on its crest; at 4 kHz that understates amplitude by 0.3 dB.
- **Deconvolution output must be trimmed to the recording length.** Circular
  transform wrap put negative-time content at the buffer's far end and EDT read
  1083 seconds.
- **Pre-warp bilinear poles.** The 12194 Hz weighting pole is halfway to Nyquist
  at 48 kHz. See `prewarp()` in `src/dsp/biquad.cpp`.
- **A peaking band at 0 dB is a pass-through; a highpass at 0 dB is not.** The
  transparency shortcut must not skip pass and reject shapes.
- **Ganged octave EQ faders overshoot ~1.8 dB.** Inherent to constant-Q, not a
  defect. Documented, not asserted away.
- **The C header is hand-written, so nothing regenerates it and nothing warns
  when it drifts.** What catches drift: the FFI implementation compiles against
  a C++ view of the header generated at configure time (a drifting signature is a
  compile error, and `src/ffi/CMakeLists.txt` checks every declaration was
  rewritten); `tests/ffi/c_compile_test.c` compiles it as C11 and C23 with
  warnings as errors and links every entry point from C; and the apps' CI builds
  Swift against it. The header is plain C with no C++ guards, and the Swift build
  passes `-Xcc -std=c23`; an enum is declared once as `enum X : uint32_t` under
  C23 and as a typedef before it, so Swift does not see two candidates.
- **clang-format will mangle `CMakeLists.txt`** if pointed at it - it formats
  CMake as C++ and rewrites the comments. A careless run did this to
  `src/engine/CMakeLists.txt`. `check.sh` and CI select `*.hpp`, `*.cpp` and
  `*.mm` under `src` and `tests` only; keep it that way, and never run it on a
  glob that includes CMake files.
- **Apple's libc++ hides APIs newer than the deployment target.**
  `std::from_chars` for floating point arrived in libc++ with macOS 26; the apps
  target macOS 14 and iOS 17, so calibration parsing failed to compile there
  while Linux accepted it. Float parsing goes through `base/number_text`, built
  on `strtod`. CI builds macOS with `CMAKE_OSX_DEPLOYMENT_TARGET=14.0` for this
  reason; check an unfamiliar library API against it.
- **FMA contraction differs by platform, hence `-ffp-contract=off`.** AppleClang
  fuses `a * b + c` on arm64 by default and GCC on x86-64 does not, so 24-bit
  pink noise differed from the golden fixture in the last bit. The flag is set
  for the whole build in `cmake/AnalyzerModule.cmake`, third-party code
  included. Do not remove it for speed.
- **Windows refuses to delete a file that is still open.** A CLI test removed a
  file it still held; Linux and macOS allowed it. Close, then delete.
- **`-Wnull-dereference` false positives at -O2.** GCC computes it after inlining,
  so in Release it fired inside libstdc++'s stream code on correct callers. It
  is deliberately off. More generally, an optimiser-only warning cannot hide
  from a Debug-only build, which is why CI builds Release on Linux.
- **A replacement `operator new` must be linked as an object, not pulled from a
  static library**, or the linker may never look for it. `analyzer::alloc_trap`
  is an `OBJECT` library for that reason.
- **`libanalyzer.a` must list every module.** The apps link one archive, merged
  from the per-module ones by `cmake/AnalyzerBundle.cmake`. When `base` became a
  real library and the list did not include it, both apps failed to link. A new
  module or library needs adding to `ANALYZER_BUNDLE_LIBRARIES`.
- **miniDSP negates the biquad feedback coefficients.** Its difference equation
  adds the terms the standard form subtracts. Exporting without flipping `a1`
  and `a2` yields an unstable filter, and the file looks correct.
- **Captured traces must outlive the session.** Transform size, window and
  averaging all restart it; a trace store inside the session loses the "before"
  curve exactly when it is wanted. `AnalyzerTraceStore` is its own handle.
- **Never boost a room null.** A dip is usually a cancellation and gain does not
  fill it, it only burns headroom. The optimiser's boost cap defaults far below
  its cut cap.
- **Float to integer needs `saturating_cast`.** `static_cast` from a float outside
  the target's range - an infinity, a NaN, a huge bin index - is undefined
  behaviour, and the UBSan build aborts on it. `base/numeric.hpp` has the one
  conversion; do not write another.

## Editing

Large multi-hunk edits have gone best as Python patch scripts
**written to a file first, then run** - a heredoc that aborts on an anchor
mismatch leaves the file untouched and forces a full re-run.

Module sources are globbed, so a new `.cpp` in a module's directory needs no
CMake edit. `src/audio/` and `src/cli/` list theirs explicitly, because which
files compile depends on the platform.

## Where things stand

The C++ core is complete and at parity with the Rust it replaced: every Rust
test was ported (the suite has 762 ctest entries), the golden outputs recorded
from the Rust build reproduce, and the C header is declaration-for-declaration
the one cbindgen generated. Both apps build against it.

Milestone 1 (real-time analyzer) is complete. Milestone 2 (swept measurement) is
complete and driven from the app as well as the CLI. Milestone 3 (EQ) is
complete: graphic and parametric equalisers, target curves, the automatic PEQ
optimiser, and filter export to REW, Equalizer APO and miniDSP.

The [macOS client](https://github.com/rossb468/analyzer-macos) is a sidebar of
sections (RTA, Transfer, Measure, Spectrogram, Equaliser, Traces) with a
per-section inspector and a standard Settings window.

The [iOS client](https://github.com/rossb468/analyzer-ios) has the same sections
as chips under the plot, for iPhone and iPad. Its audio is `src/audio/ios.mm`:
RemoteIO plus an `AVAudioSession` in measurement mode, one input port per
device. CI cross-compiles the core for iOS from `core.yml` and the app builds and
launches in the simulator; it has not yet been looked at on a phone.

**Not started:** scope view, group delay, minimum-phase decomposition, and the
Windows and Linux clients.

**The one unmet plan commitment** is the REW parity run: ±0.1 dB on synthetic
signals and ±0.5 dB on a real measurement, 20 Hz to 20 kHz. Internal consistency
is verified, which is a weaker claim than parity.

Everything on this side is built: `--generate` writes the test signals,
`--compare` measures agreement and gates on it, and `docs/REW-PARITY.md` is the
procedure. What remains is REW's own import and export, which is manual until
its API on `localhost:4735` is used to automate it. Read that document before
attempting the run - most of the difficulty is in matching REW's analysis
settings, not in the comparison.
