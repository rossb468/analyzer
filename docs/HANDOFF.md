# Project handoff

The longer narrative behind `CLAUDE.md`. That file is loaded into every session
and stays deliberately short — disciplines and traps only. This one is read when
someone needs the reasoning: why the architecture is shaped this way, what has
actually been built, what has not, and what to do next.

**Read `CLAUDE.md` first.** It carries the rules. This carries the context.

---

## What this is

A ground-up native replacement for [Room EQ Wizard][rew], the free standard for
room and loudspeaker acoustics. REW is a Java/Swing application whose local
install was inspected before any code was written: an x86-only install4j
launcher running under Rosetta, a bundled JRE from 2018, ~2,000 obfuscated
classes, and its own logs recording a `SIGSEGV` inside `libawt_lwawt.dylib` at
`OGLSD_SetScratchSurface` — the Java2D OpenGL path failing against a modern
graphics stack. Its documentation advises disabling anti-aliasing "for faster
drawing". Every plot is CPU-rasterised and there is no real-time-safe audio
path.

Open Sound Meter and Smaart cover live analysis; FuzzMeasure covers swept
measurement on macOS but has no real-time analyzer. The gap is a genuinely
native, genuinely fast **live** analyzer, and that is what this starts from.

`analyzer` is a working name. Renaming later is accepted as a brute-force
change.

[rew]: https://www.roomeqwizard.com/

## Decisions already made — do not relitigate

| Decision | Why |
|---|---|
| **Rust** core, not C++ | The concurrency primitives that are genuinely dangerous to hand-roll (SPSC rings, triple buffers) come free and safe. Most code here is AI-written, which makes a compiler that rejects data races at build time matter *more*, not less: intermittent UB is the worst possible debugging loop. |
| **Native UI per platform**, not cross-platform | macOS gets Swift + SwiftUI with a real Metal renderer. |
| **macOS first, and deep** | Windows and Linux are deferred but *not designed out* — see "Portability discipline". |
| **Copy across the FFI**, not pointers into the triple buffer | Eight traces at 2,048 points is 64 KB/frame, a few microseconds of memcpy. The pointer version bought nothing and raced an async Metal upload into a torn read. |
| **Line traces CPU, waterfall GPU** | A trace is ~2k points; that is nothing at 120 fps. A Retina waterfall composited CPU-side is 18 MB/frame, over 2 GB/s at 120 fps — a restatement of REW's own problem. |
| **No REW interop yet** | But nothing that would foreclose it. `.mdat` is Java object serialisation; never plan to parse it. The realistic bridges are REW's text exports and its API on `localhost:4735`. |
| **ASIO out of scope** | Windows-only, and the sole place a C++ shim was unavoidable. The project is pure Rust + Swift. |

## Working on it

```bash
export PATH="/opt/homebrew/opt/rustup/bin:$PATH"   # not ~/.cargo/bin
```

| Task | Command |
|---|---|
| The gate — fmt, clippy, test | `./check.sh` |
| Build and run the macOS app | `./apps/macos/build.sh --run` |
| Headless harness | `cargo run -p analyzer-cli -- --help` |
| Performance | `cargo run --release -p analyzer-cli -- --bench` |
| End-to-end sanity | `cargo run --release -p analyzer-cli -- --measure-demo` |

`./check.sh` must be green before every commit. It is not decorative: three
broken commits shipped before it was fixed, because the original piped `cargo
test` and `PIPESTATUS` reported success from a failing suite.

Conventional Commits, scope is the crate name. Short-lived feature branches,
rebase or squash onto `main`, no PRs needed. Every commit must build and pass
tests — this is what makes the history bisectable, and it is intended to be
public.

## Architecture

```
crates/analyzer-dsp/     the maths. No I/O, no OS deps.
crates/analyzer-cal/     calibration chain, dBFS to absolute dB SPL
crates/analyzer-audio/   AudioBackend trait + CoreAudio implementation
crates/analyzer-engine/  RT graph, lock-free ring, snapshot publication
crates/analyzer-model/   measurements, formats, filter export, settings
crates/analyzer-plot/    display reduction + axis transforms. Emits no pixels.
crates/analyzer-ffi/     staticlib, C ABI, cbindgen-generated header
tools/analyzer-cli/      headless harness, live capture, bench, sweep measure
```

The clients live in their own repositories and consume this one as a pinned
submodule. Today that is [analyzer-macos][macos]: Swift, SwiftUI and two Metal
renderers. Bumping the pin is a commit in the client, so which core a given app
build was made against is recorded rather than implied.

Splitting them is what makes "check out and build the core alone" true rather
than aspirational, and the core's CI runs on Linux, Windows and macOS to keep
it that way. `analyzer-ffi` is the exception: it binds to CoreAudio directly and
so is macOS-only until a second backend exists. Making it portable is part of
the port work, not a loose end.

[macos]: https://github.com/rossb468/analyzer-macos

The dependency graph is acyclic and deliberately shallow: `dsp`, `cal` and
`plot` are leaves; `engine` depends on `dsp`; `model` depends on `dsp`; `ffi`
depends on everything. Nothing depends on `ffi` except the app.

### The four rules that decide the port

These are in `CLAUDE.md` as rules. Here is why each exists.

**The audio callback does only: generate, write output, write into the ring,
return.** No allocation, no locks, no logging, no syscalls. `assert_no_alloc`
aborts the process on any allocation in debug and test builds; in release,
`AllocTrap` aliases `std::alloc::System` because `assert_no_alloc` compiles its
allocator away and a binary still needs a `#[global_allocator]`. The analysis
thread is a different matter — it may lock, and does, for the measurement
recording buffer.

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

**No Apple SDK types in the core.** Enforced by the crate graph and, since the
split, by CI: only `analyzer-audio` may depend on an Apple crate, and only
behind `cfg(target_os = "macos")`. Linux is where this breaks first, because it
has no audio stack, no window server and no Apple SDK.

**Storage is unsmoothed, complex, at native sample rate.** Smoothing and
fractional-octave banding are view transforms. Storing smoothed magnitude
forecloses group delay, RT60, minimum-phase decomposition and REW interop. An
unmeasured SPL offset stays `None` rather than becoming zero — "not measured"
and "measured as needing no correction" are different facts, and the settings
format, the FFI and the measurement container all preserve that distinction.

## Where things stand

62 commits, 506 tests, `./check.sh` green. 7 crates, 75 FFI entry points, 19
Swift files.

**Milestone 0 — headless core.** Complete except its numeric exit criterion.
See "The one unmet commitment".

**Milestone 1 — real-time analyzer. Complete.** All eleven items: device
enumeration, generator, calibration, RTA, fractional-octave bands, level meters
with A/C/Z weighting, dual-FFT transfer function with coherence, GCC-PHAT delay
finder, log axis and smoothing, and MTW.

**Milestone 2 — swept measurement. Complete**, and now driven from the app as
well as the CLI. Farina sweep, regularised deconvolution, gating, Schroeder
integration with Lundeby truncation, EDT/T20/T30, harmonic distortion. Verified
end to end against a synthetic room: arrival 10.00 ms against 10.00 built, all
three reflections within 0.02 ms, T30 0.421 s against 0.450.

**Milestone 3 — EQ. Complete.** Graphic and parametric equalisers on one shared
RBJ biquad, target curves, the automatic PEQ optimiser, and filter export to
REW, Equalizer APO and miniDSP.

**The macOS app** is a sidebar of sections — RTA, Transfer, Measure,
Spectrogram, Equaliser, Traces — with a per-section inspector and a standard
`Settings` scene. Trace capture and overlay and the running spectrogram are
done. Live capture is verified against real hardware.

**Measured performance** (`--bench`, M1 Pro). Every target passes with three to
four orders of magnitude of headroom: spectrum at 16384 points 0.0029 duty,
transfer function 0.0125, a 131072-point FFT 0.0182 duty at 55× realtime, ring
soak with zero overruns and a worst callback of 0.4 µs against a 2667 µs
budget.

**Not started:** scope view (time-domain view of live inputs), group delay,
minimum-phase decomposition, and the Windows and Linux clients.

## Requirements

### The one unmet commitment

**The REW parity run has never been executed.** The exit criterion is ±0.1 dB
against REW on synthetic signals and ±0.5 dB on a real measurement, 20 Hz to
20 kHz. The harness exists — feed identical WAV input through `analyzer-cli`
and through REW, compare the exported magnitude — and internal consistency is
verified, which is a strictly weaker claim.

This was Milestone 0's gate and three milestones have been built past it. If
parity turns up a systematic offset — a window amplitude correction, an FFT
scaling factor, a calibration-chain constant — it sits underneath everything
already built, and the EQ and optimiser work inherits it. It gets cheaper to
run and more expensive to have skipped with every milestone.

### Performance targets

Falsifiable, and all currently met. Re-run `--bench` after touching the engine.

| Target | Threshold |
|---|---|
| Analysis thread, full chain at 48 kHz | < 50% duty on one performance core |
| Ring overruns over a one-hour run | **zero** |
| Allocations on the audio thread | **zero**, CI-enforced |
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
3. The measurement container is versioned already — retrofitting a format after
   users have data is the worst version of that problem.
4. Never hard-code sample rate or channel count. REW 5.40 supports 24 output
   channels.
5. Keep the model able to emit REW-compatible text export exactly.

## Future direction

Roughly in the order they earn their keep.

1. **Run the REW parity check.** See above. Everything else is building on an
   unverified foundation.
2. **Scope view.** The one Milestone 1 item never built. A time-domain view of
   live inputs is how you find a clipping preamp or a dead channel, and right
   now the app cannot show you the waveform at all.
3. **Group delay and minimum-phase decomposition.** Both fall out of the
   impulse response the measurement path already produces. Minimum-phase
   decomposition is what tells you whether a dip is worth correcting at all —
   which would make the optimiser's boost cap an informed decision rather than
   a blanket rule.
4. **Windows and Linux clients.** Only once macOS is deep enough to be worth
   porting. The `AudioBackend` and `Fft` traits each have exactly one
   implementation, which looks like overhead and is the cheapest portability
   insurance available.

Smaller things worth doing: a loopback self-test (transfer function ≈ 0 dB,
≈ 0° phase, coherence ≈ 1.0 across the band, as a one-click check); saving and
reloading captured traces, which currently live only for the session; and
letting the measurement path write into the measurement store so a sweep can be
saved like an RTA capture.

## Things that will bite

Beyond the trap list in `CLAUDE.md`:

- **CI does not cover feature branches.** Workflows trigger on
  `push: branches: [main]` and `pull_request`, so a pushed feature branch runs
  nothing. Local `check.sh` is the only signal until merge.
- **Cross-compiling is not cross-testing.** `cargo clippy --target
  x86_64-unknown-linux-gnu` proves the core builds; it cannot run a Linux
  binary. A CLI test asserting an exit code passed that check and failed on
  Ubuntu and Windows immediately. Only the matrix catches runtime differences.
- **An FFI change is now a two-repository change.** The C ABI lives here and
  Swift compiles against it there, so a change that satisfies Rust can break the
  client, and cbindgen rewrites the header silently. The client's CI is what
  notices. Bump its submodule pin in the same sitting or the break is somebody
  else's surprise.
- **The UI has never been visually verified by an agent.** Screen recording and
  accessibility permissions are not granted to the terminal, so agent sessions
  can confirm the app builds, launches and does not crash, and nothing more. A
  selection bug that made every sidebar row unclickable compiled and ran
  cleanly. Ask the human to look. This now lives in the client repository, whose
  `CLAUDE.md` says the same thing.
- **The measurement's arrival time includes the converter round trip.** The
  sweep is armed and the recording started as two operations with nothing
  synchronising them to a sample. Correcting it needs a loopback reference,
  which is a measurement in its own right. The reading says so; keep it saying
  so.

## Before a public push

The repository is already public. These were listed as prerequisites and do not
exist yet: `CONTRIBUTING.md`, a code of conduct, issue and PR templates, and a
published changelog. Licensing is done — dual MIT OR Apache-2.0, both files
present, `license` set in every `Cargo.toml` from the first commit, and the one
dependency worth noting is `triple_buffer` at MPL-2.0, which is file-level
copyleft and explicitly permits linking from a differently-licensed larger work.
FFTW stays excluded on licence grounds.
