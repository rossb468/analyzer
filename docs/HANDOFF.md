# Project handoff

The longer narrative behind `CLAUDE.md`. That file is loaded into every session
and stays deliberately short - disciplines and traps only. This one is read when
someone needs the reasoning: why the architecture is shaped this way, what has
actually been built, what has not, and what to do next.

**Read `CLAUDE.md` first.** It carries the rules. This carries the context.
`docs/READING-ORDER.md` is the tour of the code itself, and
`docs/CPP-CONVENTIONS.md` is how C++ is written here.

---

## What this is

A ground-up native replacement for [Room EQ Wizard][rew], the free standard for
room and loudspeaker acoustics. REW is a Java/Swing application whose local
install was inspected before any code was written: an x86-only install4j
launcher running under Rosetta, a bundled JRE from 2018, ~2,000 obfuscated
classes, and its own logs recording a `SIGSEGV` inside `libawt_lwawt.dylib` at
`OGLSD_SetScratchSurface` - the Java2D OpenGL path failing against a modern
graphics stack. Its documentation advises disabling anti-aliasing "for faster
drawing". Every plot is CPU-rasterised and there is no real-time-safe audio
path.

Open Sound Meter and Smaart cover live analysis; FuzzMeasure covers swept
measurement on macOS but has no real-time analyzer. The gap is a genuinely
native, genuinely fast **live** analyzer, and that is what this starts from.

`analyzer` is a working name. Renaming later is accepted as a brute-force
change.

[rew]: https://www.roomeqwizard.com/

## Decisions already made - do not relitigate

| Decision | Why |
|---|---|
| **C++20** core, ported from an earlier Rust one | See "The port from Rust". The short version: the code is meant to be read, maintained and studied by someone who works in C++, and C++ is what the audio industry writes. The cost is that the compiler no longer rejects data races, which is paid for with hand-checked memory orders, sanitizers in CI and the allocation trap. |
| **Native UI per platform**, not cross-platform | macOS gets Swift + SwiftUI with a real Metal renderer. |
| **macOS first, and deep** | Windows and Linux are deferred but *not designed out* - see "Portability discipline". |
| **Copy across the FFI**, not pointers into the triple buffer | Eight traces at 2,048 points is 64 KB/frame, a few microseconds of memcpy. The pointer version bought nothing and raced an async Metal upload into a torn read. |
| **Line traces CPU, waterfall GPU** | A trace is ~2k points; that is nothing at 120 fps. A Retina waterfall composited CPU-side is 18 MB/frame, over 2 GB/s at 120 fps - a restatement of REW's own problem. |
| **No REW interop yet** | But nothing that would foreclose it. `.mdat` is Java object serialisation; never plan to parse it. The realistic bridges are REW's text exports and its API on `localhost:4735`. |
| **ASIO out of scope** | Windows-only, and it belongs with the Windows client. A C++ core would not need a language shim for it, but nothing is planned. |
| **Vendored dependencies, no network at build time** | KissFFT, dr_wav and GoogleTest are copied into `third_party/`. The exact code compiled is in the history, and a build works offline. |

## Working on it

Needs CMake 3.24+ and a C++20 compiler (GCC 13, Clang 18, AppleClang or MSVC),
and clang-format for the gate.

```bash
cmake -S . -B build/dev
cmake --build build/dev -j8
ctest --test-dir build/dev -j8 --output-on-failure
```

| Task | Command |
|---|---|
| The gate - format, build, test plain and under ASan+UBSan | `./check.sh` |
| Build and run the macOS app | in the [analyzer-macos][macos] repository: `./build.sh --run` |
| Headless harness | `build/dev/src/cli/analyzer-cli --help` |
| Performance | `build/rel/src/cli/analyzer-cli --bench` (a Release build) |
| End-to-end sanity | `build/dev/src/cli/analyzer-cli --measure-demo` |

`./check.sh` must be green before every commit. It is not decorative: three
broken commits shipped before it was fixed, because the original piped the test
command and `PIPESTATUS` reported success from a failing suite.

Conventional Commits, scope is the module name. Short-lived feature branches,
rebase or squash onto `main`, no PRs needed. Every commit must build and pass
tests - this is what makes the history bisectable, and it is intended to be
public.

## Architecture

```
include/analyzer.h   the C ABI; hand-maintained; the contract with the apps
src/base/            contract checks, saturating_cast, Rust-compatible number text
src/dsp/             the maths. No I/O, no OS deps.
src/cal/             calibration chain, dBFS to absolute dB SPL
src/plot/            display reduction + axis transforms. Emits no pixels.
src/model/           measurements, formats, filter export, settings, WAV I/O
src/engine/          RT graph, lock-free ring, snapshot publication, alloc trap
src/audio/           AudioBackend + offline, CoreAudio (macOS), RemoteIO (iOS)
src/ffi/             the C ABI's implementation
src/cli/             headless harness, live capture, bench, sweep measure
tests/               one GoogleTest executable per module; tests/golden fixtures
third_party/         KissFFT, dr_wav, GoogleTest
```

One static library per module, `analyzer::<module>` in CMake. The clients live in
their own repositories and consume this one as a pinned submodule. Today that is
[analyzer-macos][macos] and [analyzer-ios][ios]: Swift, SwiftUI and two Metal
renderers each. Bumping the pin is a commit in the client, so which core a given
app build was made against is recorded rather than implied.

Splitting them is what makes "check out and build the core alone" true rather
than aspirational, and the core's CI runs on Linux, Windows and macOS to keep it
that way. That includes `src/ffi`: it reaches hardware only through
`audio::default_backend()`, which picks the platform's backend and, where none
exists yet, an `UnavailableBackend` that lists no devices and refuses to open.
The C ABI therefore compiles and passes its tests everywhere.

What the apps link is `libanalyzer.a`: swiftc knows nothing about CMake targets,
so `cmake/AnalyzerBundle.cmake` merges every module the C ABI needs into one
archive with Apple's `libtool` (the `analyzer_bundle` target, Apple platforms
only for now). A module missing from that list is a link error in both apps.

[macos]: https://github.com/rossb468/analyzer-macos
[ios]: https://github.com/rossb468/analyzer-ios

The dependency graph is acyclic and deliberately shallow: `base` is under
everything; `dsp`, `cal` and `plot` are leaves above it; `engine` depends on
`dsp`; `model` depends on `dsp`; `audio` stands alone; `ffi` depends on all of
them. Nothing depends on `ffi` except the app. CMake enforces it - a module that
includes something it does not link against does not compile.

### The four rules that decide the port

(Here "the port" means the future ports to Windows and Linux, not the port from
Rust.) These are in `CLAUDE.md` as rules. Here is why each exists.

**The audio callback does only: generate, write output, write into the ring,
return.** No allocation, no locks, no logging, no syscalls. The callback body
runs inside `engine::rt_section()`; `analyzer::alloc_trap` replaces the global
`operator new` with one that aborts if it runs inside a section. The test
binaries for `engine`, `audio`, `ffi` and `cli` link it, and so does the Debug
CLI, so any test that drives a callback is held to the rule. A Release build does
not link it and `rt_section` does nothing. The analysis thread is a different
matter - it may lock, and does, for the measurement recording buffer.

**No application logic in the clients.** Analysis config, unit conversion,
smoothing, axis scaling, trace management, calibration and file I/O live here. A
client must never compute a bin-to-pixel mapping; it calls `analyzer_freq_to_x`,
`analyzer_x_to_freq`, `analyzer_db_to_y`, `analyzer_y_to_db`,
`analyzer_phase_to_y`, `analyzer_coherence_to_y`. This single discipline decides
whether the Windows and Linux ports are weeks or months, and the temptation to
break it peaks exactly when moving fast.

Separate repositories make this harder to break by accident, but they do not
enforce it: a client can always reimplement the maths locally. The check is that
every number on screen came from a call, not a calculation.

The one arithmetic a client is allowed is **pixels to points**, because that is
a platform coordinate-space concern rather than an analysis one.

**No Apple SDK types in the core.** Enforced by the module graph and by CI: only
`src/audio/` may include an Apple header, and only behind the guards in
`audio/target.hpp` and the platform branches of `src/audio/CMakeLists.txt`
(`coreaudio.cpp` on macOS, `ios.mm` for iOS). Linux is where this breaks first,
because it has no audio stack, no window server and no Apple SDK. The
Objective-C++ iOS backend cannot be compiled from Linux at all, so the core's
macOS CI job cross-compiles it for an iPhone SDK; that is the only place it is
built before the app's own CI sees it.

**Storage is unsmoothed, complex, at native sample rate.** Smoothing and
fractional-octave banding are view transforms. Storing smoothed magnitude
forecloses group delay, RT60, minimum-phase decomposition and REW interop. An
unmeasured SPL offset stays an empty `std::optional` rather than becoming zero -
"not measured" and "measured as needing no correction" are different facts, and
the settings format, the FFI and the measurement container all preserve that
distinction.

## The port from Rust

The core was first written in Rust (seven crates and a CLI) and then ported to
C++20, module by module, with the Rust deleted at the end. This section records
why and how, because the shape of the C++ follows from it.

**Why.** Two reasons, both about the people reading the code. The owner works in
C++ and not in Rust, and a codebase that its owner cannot comfortably read, debug
or extend is a poor foundation however good its type system; maintainability and
readability won. And the project is also a study vehicle for real-time audio
DSP, where C++ is what the industry writes, so the ring buffer, the lock-free
snapshot, the allocation guard and the FFT wrapper are worth having as C++ that
can be read, discussed and defended. The Rust version's argument - the compiler
rejects data races, which matters when much of the code is machine-written - was
real, and its price is now paid differently: ThreadSanitizer, AddressSanitizer
and UBSan run over the whole suite in CI, contracts abort in every build, and
the lock-free pieces are small enough to review by eye.

**How.** One module at a time, each on its own branch (`cpp/dsp-filters`,
`cpp/dsp-spectrum`, `cpp/engine`, `cpp/audio`, `cpp/model`, `cpp/cli`,
`cpp/ffi`, ...) and merged only when green, in the dependency order of the
crates, with the Rust still building beside it until `cpp/remove-rust`. Two
things held the C++ to the Rust:

- **The tests were the specification.** Every Rust test was ported - same name
  in `PascalCase`, same assertions, same tolerances, same comments - so what each
  function promises and which bug each test came from carried over, not just the
  code. A tolerance was never loosened to make a port pass without understanding
  why it differed.
- **Golden outputs were recorded from the Rust build before it was deleted.**
  `tests/golden/` holds CLI output for every command that is reproducible,
  measurement files and exports the CLI cannot produce, and a manifest saying how
  strictly each is compared (byte-exact, or within a stated tolerance where a
  different FFT moves the last digits). `tests/cli/golden_test.cpp` and
  `tests/model/golden_test.cpp` replay it; their test suite is still called
  `MatchesTheRustOutput`. The last revision containing the Rust is `e9a459b`;
  `tests/golden/README.md` says how to regenerate from it. The C header was
  checked token for token against what cbindgen generated before that check was
  removed along with the Rust.

**What changed in the design**, beyond syntax:

- **Three kinds of failure, three mechanisms.** Rust had `Result` for setup
  errors and panics for bugs. Here a broken precondition is
  `ANALYZER_EXPECTS` (`base/contract.hpp`), which aborts with a message in every
  build and is tested with death tests; a setup failure (a file that does not
  parse, a device that has gone) throws a module exception derived from
  `std::runtime_error`; and anything on the audio or analysis thread is
  `noexcept` and reports conditions by return value. The C ABI catches every
  exception at the boundary, as the Rust caught panics.
- **The lock-free primitives are hand-written.** Rust took `rtrb` and
  `triple_buffer`; C++ has no equivalent in the standard library, and the two
  are small. `engine/ring.hpp` is a single-producer single-consumer ring of two
  monotonic counters with acquire/release pairs and no compare-and-swap, and
  `engine/snapshot.hpp` is a triple buffer whose publish and read are one atomic
  exchange each. Weakening the ring's release store to relaxed makes the suite
  fail under ThreadSanitizer, so the threading tests can see the bug they exist
  for. Move-only endpoints make "exactly one producer" a compile-time fact.
- **The allocation trap is a replacement global `operator new`.** Rust used the
  `assert_no_alloc` crate and had to alias the allocator in release. C++ lets a
  program replace `operator new` outright, so `alloc_trap.cpp` is the whole
  mechanism, linked only into the binaries that want it.
- **KissFFT sits behind `dsp::Fft`.** It replaced `realfft`: any even size, no
  allocation per transform (its scratch is on the stack), plain C that can be
  read end to end. It is confined to `fft.cpp`, behind a pimpl (`RealFft::Plans`)
  so that no other file can include it, and Apple's vDSP or pffft would slot in
  as another `Fft`.
- **The C header is hand-written.** cbindgen generated it from the Rust; now
  `include/analyzer.h` is the source of truth and the contract with the apps.
  The implementation compiles against a C++ view of it that CMake generates at
  configure time, so drift is a compile error rather than a surprise in Swift;
  the tests compile it as C11 and C23 and link every entry point from C.
- **Small shared pieces the Rust got from its language.** `saturating_cast`
  (`base/numeric.hpp`) reproduces what Rust's `as` does for out-of-range floats,
  which in C++ is undefined behaviour. `base/number_text` reproduces Rust's
  `{}` and `{:.N}` float formatting and `str::parse` on top of `std::to_chars`
  and `strtod`, so that text files are byte-identical between the two
  implementations and the exports can be checked against fixtures.
- **Build discipline.** `-ffp-contract=off` everywhere, because fused
  multiply-add differs by platform and the goldens compare bits; warnings as
  errors; Debug builds with the standard library's bounds checks on; and CI
  builds for the apps' real deployment targets (macOS 14, iOS 17), because
  Apple's libc++ hides APIs newer than that.
- **Engine departures, each for the C++ reader.** `DelayLine` has its own header
  and tests; the initial snapshot reserves the transfer vectors so the worker
  never allocates once running; `begin_recording()` allocates before taking the
  recording lock and `cancel_recording()` frees after releasing it.

## Where things stand

762 tests under ctest, `./check.sh` green; nine modules, 76 FFI entry points.

**Milestone 0 - headless core.** Complete except its numeric exit criterion.
See "The one unmet commitment".

**Milestone 1 - real-time analyzer. Complete.** All eleven items: device
enumeration, generator, calibration, RTA, fractional-octave bands, level meters
with A/C/Z weighting, dual-FFT transfer function with coherence, GCC-PHAT delay
finder, log axis and smoothing, and MTW.

**Milestone 2 - swept measurement. Complete**, and driven from the app as well
as the CLI. Farina sweep, regularised deconvolution, gating, Schroeder
integration with Lundeby truncation, EDT/T20/T30, harmonic distortion. Verified
end to end against a synthetic room: `--measure-demo` reports arrival 10.00 ms
against 10.00 built, all three reflections within 0.02 ms, T30 0.421 s against
0.450.

**Milestone 3 - EQ. Complete.** Graphic and parametric equalisers on one shared
RBJ biquad, target curves, the automatic PEQ optimiser, and filter export to
REW, Equalizer APO and miniDSP.

**The C++ core is complete and at parity with the Rust it replaced**: every
Rust test ported, golden outputs reproduced, and both apps build against it.

**The macOS app** is a sidebar of sections - RTA, Transfer, Measure,
Spectrogram, Equaliser, Traces - with a per-section inspector and a standard
`Settings` scene. Trace capture and overlay and the running spectrogram are
done. Live capture is verified against real hardware.

**The iOS app** is the same sections as chips under the plot, with the
inspector below it upright and beside it turned. Its backend is RemoteIO with an
`AVAudioSession` in measurement mode, so iOS applies no gain control or voice
processing; a device is an input port, and output follows the session's route,
which removes the aggregate-device problem macOS has. CI builds it and launches
it in the simulator. It has not yet been checked on a phone.

**Performance.** `--bench` in a Release build reports every target with large
headroom. One run on a Linux x86-64 machine (Xeon at 2.8 GHz, GCC 13, Release,
`--bench 2`) gave a spectrum at 16384 points at 0.0093 duty (108x realtime), a
two-channel transfer function at the same size at 0.045 duty (22x), A-weighted
level metering at 0.00053 duty, and a ring soak of 750 blocks with zero overruns
and a worst callback of 0.4 us against a 2667 us budget. The figures vary a
little from run to run and a lot between machines; the Rust build, measured
earlier on an M1 Pro, reported lower duty (spectrum 0.0029, transfer function
0.0125), as a faster machine would. Re-run it rather than quoting either.

**Not started:** scope view (time-domain view of live inputs), group delay,
minimum-phase decomposition, and the Windows and Linux clients.

## Requirements

### The one unmet commitment

**The REW parity run has never been executed.** The exit criterion is ±0.1 dB
against REW on synthetic signals and ±0.5 dB on a real measurement, 20 Hz to
20 kHz. Internal consistency is verified, which is a strictly weaker claim.

Every part of it on this side exists: `--generate` writes deterministic test
signals as WAV, `--compare` reads two frequency/level exports and reports how
far apart they are, and `docs/REW-PARITY.md` is the procedure. The comparison
reports a constant offset separately from the deviation that survives removing
it, because those mean different things - a constant offset is a reference
convention such as full-scale sine against full-scale square, or per-bin level
against power per hertz, and only a frequency-dependent deviation is a defect.

What is missing is REW's own half: importing each file and exporting its
measurement. That is manual, because REW's GUI cannot be driven from a terminal
session; its API on `localhost:4735` would automate it. Most of the remaining
difficulty is in matching REW's analysis settings rather than in the comparison,
and `docs/REW-PARITY.md` lists the ones that decide whether the run means
anything.

This was Milestone 0's gate and three milestones have been built past it. If
parity turns up a systematic offset - a window amplitude correction, an FFT
scaling factor, a calibration-chain constant - it sits underneath everything
already built, and the EQ and optimiser work inherits it. The port did not move
it: the C++ reproduces the Rust's outputs, so a parity error in one is a parity
error in both. It gets cheaper to run and more expensive to have skipped with
every milestone.

### Performance targets

Falsifiable, and all currently met. Re-run `--bench` (Release) after touching the
engine.

| Target | Threshold |
|---|---|
| Analysis thread, full chain at 48 kHz | < 50% duty on one performance core |
| Ring overruns over a one-hour run | **zero** |
| Allocations on the audio thread | **zero**, enforced by the allocation trap in the test suite |
| UI frame rate, 8 traces + coherence + spectrogram | 120 fps ProMotion, 60 fps floor |
| Audio-in to display frame containing it | ≤ 50 ms at a 128-frame buffer |
| Idle CPU, generator and analyzer stopped | < 1% **and no timer wakeups** |

The last one is why the renderer skips frames with no new data rather than
redrawing on a free-running timer.

### REW-compatibility guardrails

No interop is being built, but these cost nothing now and are expensive to
retrofit:

1. Store complex spectra and impulse responses at native rate, unsmoothed.
2. Preserve absolute references in metadata: time-zero, full-scale voltages,
   reference resistance, SPL offset.
3. The measurement container is versioned already - retrofitting a format after
   users have data is the worst version of that problem.
4. Never hard-code sample rate or channel count. REW 5.40 supports 24 output
   channels.
5. Keep the model able to emit REW-compatible text export exactly.

## Future direction

Roughly in the order they earn their keep.

1. **Run the REW parity check.** See above. Everything else is building on an
   unverified foundation, and the only part still missing is REW's own export.
2. **Scope view.** The one Milestone 1 item never built. A time-domain view of
   live inputs is how you find a clipping preamp or a dead channel, and right
   now the app cannot show you the waveform at all.
3. **Group delay and minimum-phase decomposition.** Both fall out of the
   impulse response the measurement path already produces. Minimum-phase
   decomposition is what tells you whether a dip is worth correcting at all -
   which would make the optimiser's boost cap an informed decision rather than
   a blanket rule.
4. **Windows and Linux clients.** Only once macOS is deep enough to be worth
   porting. `audio::AudioBackend` and `dsp::Fft` each have one real
   implementation behind an abstract class, which looks like overhead and is the
   cheapest portability insurance available. `AnalyzerBundle.cmake` will want
   `lib.exe` and `ar` equivalents of the `libtool` merge.

Smaller things worth doing: a loopback self-test (transfer function ≈ 0 dB,
≈ 0° phase, coherence ≈ 1.0 across the band, as a one-click check); saving and
reloading captured traces, which currently live only for the session; and
letting the measurement path write into the measurement store so a sweep can be
saved like an RTA capture.

## Things that will bite

Beyond the trap list in `CLAUDE.md`:

- **CI does not cover feature branches.** The workflow triggers on
  `push: branches: [main]` and `pull_request`, so a pushed feature branch runs
  nothing. Local `check.sh` is the only signal until merge. It also does not run
  Release or ThreadSanitizer; those are CI-only, on Linux.
- **Cross-compiling is not cross-testing.** The iOS build proves the Objective-C++
  backend compiles against an iPhone SDK; it cannot run it. A CLI test that
  deleted a file it still held open passed on Linux and macOS and failed on
  Windows immediately. Only the three-platform matrix catches runtime
  differences, and the MSVC warning set is shown but not yet held to zero.
- **An FFI change is a three-repository change.** The C ABI lives here and Swift
  compiles against it in two other repositories, so a change that satisfies the
  core's own tests can still break a client. The apps' CI is what notices. Bump
  their submodule pins in the same sitting or the break is somebody else's
  surprise.
- **The UI has never been visually verified by an agent.** Screen recording and
  accessibility permissions are not granted to the terminal, so agent sessions
  can confirm the app builds, launches and does not crash, and nothing more. A
  selection bug that made every sidebar row unclickable compiled and ran
  cleanly. Ask the human to look. This lives in the client repositories, whose
  `CLAUDE.md` files say the same thing.
- **The measurement's arrival time includes the converter round trip.** The
  sweep is armed and the recording started as two operations with nothing
  synchronising them to a sample. Correcting it needs a loopback reference,
  which is a measurement in its own right. The reading says so; keep it saying
  so.

## Before a public push

The repository is already public. These were listed as prerequisites and do not
exist yet: `CONTRIBUTING.md`, a code of conduct, issue and PR templates, and a
published changelog. Licensing is done - dual MIT OR Apache-2.0, both files
present - and every vendored dependency is permissive: KissFFT (BSD-3-Clause),
GoogleTest (BSD-3-Clause, tests only) and dr_wav (public domain or MIT-0), with
their licence files beside the code in `third_party/`. FFTW stays excluded on
licence grounds.
