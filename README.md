# analyzer

A native real-time audio and acoustic measurement tool. Rust core, native UI per
platform, macOS first.

> **Status: pre-alpha.** The DSP core, audio backend and macOS app all build and
> run, and live capture is verified against real hardware. The real-time
> analyzer, the dual-FFT transfer function and both equalisers work end to end.
> Nothing has been checked against REW's own numbers yet. `analyzer` is a
> working name.

## Why

[Room EQ Wizard](https://www.roomeqwizard.com/) is the free standard for room and
loudspeaker acoustics, and it is a Java/Swing application from 2005. Every plot is
CPU-rasterised through Java2D, its own documentation suggests turning off
anti-aliasing "for faster drawing", and there is no real-time-safe audio path.
[Open Sound Meter](https://github.com/psmokotnin/osm) and
[Smaart](https://www.rationalacoustics.com/) cover live analysis;
[FuzzMeasure](https://rodetest.com/) covers swept measurement on macOS but has no
real-time analyzer at all.

The gap is a genuinely native, genuinely fast **live** analyzer. That is what this
starts with.

## What works

| Area | State |
|---|---|
| FFT, windows, Welch spectrum | Checked against published correction factors |
| Signal generator | Sine, white, pink, exponential sweep |
| Transfer function | H1 estimator: magnitude, phase, coherence |
| Multi-time-window | Several FFT sizes spliced onto a log grid |
| Delay finder | GCC-PHAT with sub-sample interpolation |
| Level meters | Peak, RMS, LEQ; A/C/Z weighting; fast/slow/impulse |
| Octave bands | IEC 61260, 1/1 through 1/48 |
| Calibration | dBFS to dB SPL, mic curves, weighting |
| Harmonic distortion | THD, THD+N, per-order harmonics, Nyquist-aware |
| Biquads | RBJ cookbook: peaking, shelves, pass, notch, allpass |
| Equalisers | 10-band graphic on ISO octaves, and free parametric |
| Sweep deconvolution | Regularised, recovering an impulse response |
| Impulse analysis | Gating, gated response, Schroeder decay, EDT/T20/T30 |
| Measurement model | Types, store, versioned file format, REW text export |
| CoreAudio backend | Capture and playback, aggregate devices, verified live |
| Engine | Lock-free ring, analysis thread, snapshot publication |
| macOS app | RTA, transfer function, generator, equaliser, save and export |

## Building

Needs Rust 1.88 or newer — let-chains — and Xcode for the macOS app.

```bash
./check.sh
```

That runs format, lint and the full test suite as one gate. It exists because
hand-rolling those three commands in a shell one-liner kept swallowing exit codes.

The macOS app:

```bash
./apps/macos/build.sh --run
```

## Trying it

The headless harness runs the whole chain from a file or a synthesised signal and
needs no hardware:

```bash
cargo run -p analyzer-cli -- --help
```

A half-scale sine on an exact bin centre should read −6.02 dBFS:

```bash
cargo run -p analyzer-cli -- --sine 996.09375 --amplitude 0.5 --seconds 1 --window flattop --peak
```

Off a bin centre the window choice starts to matter, which is why there is more
than one — flat-top loses about 0.002 dB to scalloping where Hann loses 0.63:

```bash
for w in hann bh flattop; do cargo run -q -p analyzer-cli -- --sine 1000 --amplitude 0.5 --seconds 1 --window $w --peak | grep -v '^#'; done
```

List audio devices, which works without any permission:

```bash
cargo run -p analyzer-cli -- --list-devices
```

Measure a synthetic room end to end — sweep, convolve, deconvolve, and report
arrival, reflections, reverberation time and gated response, with the
constructed truth printed alongside:

```bash
cargo run --release -p analyzer-cli -- --measure-demo
```

Or deconvolve a real pair of recordings:

```bash
cargo run --release -p analyzer-cli -- --measure stimulus.wav response.wav
```

## Performance

Measured with `--bench` on an M1 Pro. Duty cycle is CPU seconds per second of
audio; the target is under 0.5 on one performance core.

| Case | Duty | Realtime factor |
|---|---|---|
| Spectrum, 16384-point, 75% overlap | 0.0029 | 341× |
| Transfer function, 16384-point, two channels | 0.0125 | 80× |
| Level meter, A-weighted | 0.00068 | 1473× |
| Octave banding, 1/48, at 120 Hz | 0.00105 | — |

Ring soak over 1125 blocks: **zero overruns**, worst audio callback **0.2 µs**
against a 2667 µs budget. That last figure is what the deinterleave-and-return
callback design was for.

```bash
cargo run --release -p analyzer-cli -- --bench 5
```

## Architecture

```
crates/
  analyzer-dsp/       FFT, windows, spectra, transfer function, MTW, delay,
                      meters, octave bands, generator, deconvolution, impulse
                      analysis. No I/O, no platform dependencies.
  analyzer-cal/       Calibration chain: converter samples to absolute dB SPL.
  analyzer-audio/     AudioBackend trait, CoreAudio backend, offline backend.
  analyzer-engine/    Lock-free buffering, analysis thread, allocation trap.
  analyzer-model/     Measurements, store, file format, REW text export.
  analyzer-plot/      Display data reduction and axis transforms. Emits no pixels.
  analyzer-ffi/       Stable C ABI for the platform user interfaces.
apps/macos/           Swift + SwiftUI shell with a Metal renderer.
tools/analyzer-cli/   Headless harness and benchmarks.
```

The thread topology is where the performance comes from:

```
audio thread          ring         analysis thread     triple buffer    UI thread
────────────                       ───────────────                      ─────────
hard deadline    ──▶  [ring]  ──▶  heavy FFT work  ──▶  [snapshot] ──▶  draws
deinterleave                       no deadline,          newest wins     never
and return                         must keep up                          blocks
```

Decisions worth knowing about:

**The audio callback cannot allocate, and that is enforced rather than trusted.**
Every real-time audio codebase has this rule; most enforce it by code review,
which means violations ship and surface as a click once an hour on somebody
else's machine. Here the callback runs inside a guard that aborts the process,
and CI runs the same guard.

**The capture ring is interleaved and writes are all-or-nothing.** With one ring
per channel a partial write could advance one channel and not another, and
channels that drift by a single sample destroy a transfer-function phase reading.
Dropped blocks are counted and surfaced, never hidden — a measurement taken across
dropped audio is wrong, not merely noisy.

**The core emits no pixels.** Line traces are generated as geometry, but the
waterfall is one column of data per frame written into a GPU ring texture.
Recompositing a full Retina waterfall on the CPU is roughly what REW does, and it
is why its waterfall is slow.

**Axis mapping lives in Rust and is queried, never reimplemented.** Cursor
readout, hit-testing and the drawn curve have to agree exactly, and a UI doing
its own bin-to-pixel arithmetic is how they quietly stop agreeing.

**Measurements are stored unsmoothed, complex, in f64, with their absolute
references attached.** Smoothing is a view transform; storing a smoothed
magnitude curve forecloses group delay, RT60 and minimum-phase decomposition
forever. An unknown SPL offset stays `None` rather than becoming zero, because
"not measured" and "measured as needing no correction" are different facts.

## Portability

macOS comes first and deep, but the deferral is designed for rather than assumed
away: no application logic lives in Swift, `AudioBackend` and `Fft` are traits
with one implementation each, and no Apple SDK type appears outside
`apps/macos/` and one `cfg(target_os = "macos")` module.

## Not done yet

The plan's own exit criterion for the headless core is a numeric match against
REW: ±0.1 dB on synthetic signals and ±0.5 dB on a real measurement, 20 Hz to
20 kHz. **That comparison has not been run.** Internal consistency is verified
against analytically known answers throughout, which is a different and weaker
claim.

Also outstanding: scope view, trace capture and overlay, the running
spectrogram, group delay, minimum-phase decomposition, target curves, an
automatic PEQ optimiser, filter export to hardware, and the Windows and Linux
clients.

## A note on microphone permission

macOS gates capture behind TCC, and it will not raise a permission prompt for a
process launched in a non-interactive background session — it refuses outright,
and CoreAudio then stalls for minutes before failing. Run the app or the CLI once
from a foreground Terminal window, or grant access under System Settings →
Privacy & Security → Microphone. Device enumeration works without it.

Playing a stimulus and capturing it at once needs both directions on **one**
device, because a CoreAudio IOProc belongs to one device and two devices means
two clocks. A laptop's built-in input and output are separate devices, so build
an aggregate device in Audio MIDI Setup and select that.

## Licence

Dual licensed under either of

- Apache License, Version 2.0 ([LICENSE-APACHE](LICENSE-APACHE))
- MIT License ([LICENSE-MIT](LICENSE-MIT))

at your option.

One dependency, `triple_buffer`, is MPL-2.0. That is file-level copyleft and MPL
§3.3 explicitly permits linking from a differently-licensed larger work, so it is
compatible — noted because it is the only non-permissive dependency in the tree.
