# Working on `analyzer`

A native replacement for Room EQ Wizard. Rust core, native UI per platform,
macOS first and deep before any other client starts.

`docs/HANDOFF.md` is the longer narrative: why the architecture is shaped this
way, what is built, what is not, and what to do next. Read it when you need the
reasoning rather than the rules.

The original plan — milestones, performance targets, REW-compatibility
guardrails — lives at `~/.claude/plans/radiant-twirling-lerdorf.md`, outside the
repository. `docs/HANDOFF.md` carries everything from it that still matters.

## Environment

- **Cargo is at `/opt/homebrew/opt/rustup/bin`, not `~/.cargo/bin`.** Export it
  before any cargo command or the invocation fails confusingly.
- Rust 1.88+ (let-chains), edition 2024. Xcode for the app.

## Commands

```bash
export PATH="/opt/homebrew/opt/rustup/bin:$PATH"
```

| Task | Command |
|---|---|
| The gate — fmt, clippy, test | `./check.sh` |
| Headless harness | `cargo run -p analyzer-cli -- --help` |
| Performance | `cargo run --release -p analyzer-cli -- --bench` |
| End-to-end sanity | `cargo run --release -p analyzer-cli -- --measure-demo` |

`./check.sh` must be green before every commit. It is not decorative: three
broken commits shipped before it was fixed.

## Layout

```
crates/analyzer-dsp/     the maths. No I/O, no OS deps.
crates/analyzer-cal/     calibration chain, dBFS to absolute dB SPL
crates/analyzer-audio/   AudioBackend trait + CoreAudio implementation
crates/analyzer-engine/  RT graph, lock-free ring, snapshot publication
crates/analyzer-model/   measurements, versioned format, REW text export,
                         filter export, program settings
crates/analyzer-plot/    display reduction + axis transforms. Emits no pixels.
crates/analyzer-ffi/     staticlib, C ABI, cbindgen-generated header
tools/analyzer-cli/      headless harness, live capture, bench, sweep measure
```

## Rules that are not style preferences

Violating any of these is a real bug.

**The audio callback does only: generate, write output, write into the ring,
return.** No allocation, no locks, no logging, no syscalls. Everything is
preallocated at device-open. `assert_no_alloc` aborts the process on any
allocation in debug and test builds. In release `AllocTrap` aliases
`std::alloc::System`, because `assert_no_alloc` compiles its allocator away and
a binary still needs a `#[global_allocator]`.

**No application logic in the clients.** Analysis config, unit conversion,
smoothing, axis scaling, trace management, calibration and file I/O live here. A
client must never compute a bin-to-pixel mapping; it calls `analyzer_freq_to_x`,
`analyzer_x_to_freq`, `analyzer_db_to_y`, `analyzer_y_to_db`,
`analyzer_phase_to_y`, `analyzer_coherence_to_y`. This decides whether the
Windows and Linux ports are weeks or months, and the temptation to break it
peaks when moving fast.

The macOS client lives in its own repository ([analyzer-macos](https://github.com/rossb468/analyzer-macos)) and
consumes this one as a pinned submodule. Anything that changes the C ABI needs a
matching change there, and its CI is what catches the mismatch.

**No Apple SDK types anywhere in this repository.** Only `analyzer-audio` may
depend on an Apple crate, and only behind `cfg(target_os = "macos")`. Everything
else must build on Linux and Windows, which CI checks on every push.

The headless harness follows the same rule: live capture is confined to
`live_coreaudio.rs` behind a `cfg`, so WAV analysis, the bench and swept
measurement run anywhere.

**Storage is unsmoothed, complex, at native sample rate.** Smoothing and
fractional-octave banding are view transforms. Storing smoothed magnitude
forecloses group delay, RT60, minimum-phase decomposition and REW interop. An
unmeasured SPL offset stays `None` rather than becoming zero — "not measured"
and "measured as needing no correction" are different facts.

**The core emits no pixels.** Line traces are CPU-side geometry in
`analyzer-plot`. The waterfall is one column of data per frame into a GPU ring
texture. Compositing Retina-resolution pixels on the CPU is roughly what REW
does and is why its waterfall is slow.

**Dropped blocks are counted and surfaced, never hidden.** A measurement taken
across dropped audio is wrong, not merely noisy.

## Git

Conventional Commits, scope is the crate name. Short-lived feature branches
(`feat/`, `fix/`, `perf/`, `refactor/`, `chore/`), rebase or squash onto `main`,
no PRs needed. Every commit must build and pass tests. History is intended to be
made public.

## Traps this codebase has already hit

Each of these cost real time. Most now have a guard or a regression test.

- **Never pipe a test command.** `cargo test | tee | grep || true` resets
  `PIPESTATUS` and reports success from a failing suite. `check.sh` redirects to
  a file and captures the status directly.
- **`DRAIN_FRAMES` in `engine.rs` is load-bearing, not a tuning knob.** Draining
  the ring until empty lets a fast producer starve publication — 7332 frames
  analysed, 0 published. Test: `a_fast_producer_cannot_starve_publication`.
- **Feed tests a continuous stream, never a repeated block.** A repeated block
  is periodic, and that periodicity is never what the test is measuring. It has
  bitten twice: the delay finder correctly reported −3968 for a 128-sample delay,
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
  at 48 kHz. See `prewarp()` in `biquad.rs`.
- **A peaking band at 0 dB is a pass-through; a highpass at 0 dB is not.** The
  transparency shortcut must not skip pass and reject shapes.
- **Ganged octave EQ faders overshoot ~1.8 dB.** Inherent to constant-Q, not a
  defect. Documented, not asserted away.
- **cbindgen:** `cpp_compat = false`, and the Swift build passes `-Xcc -std=c23`.
  Otherwise Swift sees two candidates for every enum. cbindgen also rewrites the
  header silently, so a C ABI change this repository is happy with can break the
  client; its CI is what notices.
- **`build.sh` targets bash 3.2 under `set -u`.** Empty arrays explode; it uses
  plain strings.
- **miniDSP negates the biquad feedback coefficients.** Its difference equation
  adds the terms the standard form subtracts. Exporting without flipping `a1`
  and `a2` yields an unstable filter, and the file looks correct.
- **Captured traces must outlive the session.** Transform size, window and
  averaging all restart it; a trace store inside the session loses the "before"
  curve exactly when it is wanted. `AnalyzerTraceStore` is its own handle.
- **Never boost a room null.** A dip is usually a cancellation and gain does not
  fill it, it only burns headroom. The optimiser's boost cap defaults far below
  its cut cap.

## Editing

Large multi-hunk edits have gone best as Python patch scripts
**written to a file first, then run** — a heredoc that aborts on an anchor
mismatch leaves the file untouched and forces a full re-run.

## Where things stand

Milestone 1 (real-time analyzer) is complete. Milestone 2 (swept measurement) is
complete and driven from the app as well as the CLI. Milestone 3 (EQ) is
complete: graphic and parametric equalisers, target curves, the automatic PEQ
optimiser, and filter export to REW, Equalizer APO and miniDSP.

The [macOS client](https://github.com/rossb468/analyzer-macos) is a sidebar of sections (RTA, Transfer, Measure,
Spectrogram, Equaliser, Traces) with a per-section inspector and a standard
Settings window.

**Not started:** scope view, group delay, minimum-phase decomposition, and the
Windows and Linux clients.

**The one unmet plan commitment** is the REW parity run: ±0.1 dB on synthetic
signals and ±0.5 dB on a real measurement, 20 Hz to 20 kHz. The harness exists
and internal consistency is verified, which is a weaker claim than parity.
