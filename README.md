# analyzer

A native real-time audio and acoustic measurement tool. C++20 core, native UI per
platform, macOS first.

> **Status: pre-alpha.** The DSP core, audio backend and macOS app all build and
> run, and live capture is verified against real hardware. The real-time
> analyzer, the dual-FFT transfer function and both equalisers work end to end.
> Nothing has been checked against REW's own numbers yet. `analyzer` is a
> working name.

The core was written in Rust first and then ported to C++20, test for test; see
"The port from Rust" in [docs/HANDOFF.md](docs/HANDOFF.md). New to the code?
[docs/READING-ORDER.md](docs/READING-ORDER.md) is a guided tour.

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
| Target curves | Flat, tilt, room, or points from a file |
| PEQ optimiser | Greedy fit with local refinement, boost capped well below cut |
| Filter export | REW, Equalizer APO, miniDSP biquads |
| Measurement model | Types, store, versioned file format, REW text export |
| Signal files | Sine, noise and sweeps written as WAV, reproducibly |
| Comparison | Two exports against each other, offset separated from shape |
| CoreAudio and RemoteIO backends | Capture and playback; aggregate devices on macOS |
| Engine | Lock-free ring, analysis thread, triple-buffer snapshot publication |
| C ABI | 76 entry points in `include/analyzer.h`, used by the macOS and iOS apps |

## Building

You need CMake 3.24 or newer and a C++20 compiler (GCC 13, Clang 18, AppleClang
or MSVC), plus clang-format for the gate. Nothing is downloaded: KissFFT, dr_wav
and GoogleTest are vendored in `third_party/`.

```bash
cmake -S . -B build/dev
cmake --build build/dev -j8
ctest --test-dir build/dev -j8 --output-on-failure
```

The whole gate - format check, a build with warnings as errors, and the full
test suite both plainly and under AddressSanitizer and UBSan - is one command:

```bash
./check.sh
```

It exists because hand-rolling those steps in a shell one-liner kept swallowing
exit codes. CI runs the suite on Linux, Windows and macOS, and adds Release,
ThreadSanitizer and an iOS cross-compile.

The apps live in their own repositories and consume this one as a submodule:
[analyzer-macos](https://github.com/rossb468/analyzer-macos) and
[analyzer-ios](https://github.com/rossb468/analyzer-ios). On Apple platforms the
`analyzer_bundle` target merges every module into one `libanalyzer.a` for them.

## Trying it

The headless harness runs the whole chain from a file or a synthesised signal and
needs no hardware. In the examples below, `analyzer-cli` is
`build/dev/src/cli/analyzer-cli`.

```bash
analyzer-cli --help
```

A half-scale sine on an exact bin centre should read -6.02 dBFS:

```bash
analyzer-cli --sine 996.09375 --amplitude 0.5 --seconds 1 --window flattop --peak
```

Off a bin centre the window choice starts to matter, which is why there is more
than one - flat-top loses about 0.002 dB to scalloping where Hann loses 0.63:

```bash
for w in hann bh flattop; do analyzer-cli --sine 1000 --amplitude 0.5 --seconds 1 --window $w --peak | grep -v '^#'; done
```

Measure a synthetic room end to end - sweep, convolve, deconvolve, and report
arrival, reflections, reverberation time and gated response, with the
constructed truth printed alongside:

```bash
analyzer-cli --measure-demo
```

Or deconvolve a real pair of recordings:

```bash
analyzer-cli --measure stimulus.wav response.wav
```

Write a test signal, then analyse it and compare the result against another
analyser's export. Generation is deterministic, so the same command always
produces the same file:

```bash
analyzer-cli --generate pink --seconds 8 --out pink.wav
analyzer-cli pink.wav --fft 8192 --window hann > ours.txt
analyzer-cli --compare ours.txt theirs.txt --tolerance 0.1
```

That last command is the REW parity check; `docs/REW-PARITY.md` is the
procedure. It reports a constant offset separately from the disagreement in
shape, because the first is a reference convention and only the second is a
defect.

Live capture and device listing need a platform backend, which today means macOS:

```bash
analyzer-cli --list-devices
analyzer-cli --live 5
```

On a platform without one they say so and exit non-zero; everything else runs
anywhere. To time the analysis chain, build Release first - a Debug build links
the allocation trap and the standard library's bounds checks and says nothing
about speed:

```bash
cmake -S . -B build/rel -DCMAKE_BUILD_TYPE=Release && cmake --build build/rel -j8
build/rel/src/cli/analyzer-cli --bench 5
```

`--bench` reports the duty cycle (CPU seconds per second of audio, target under
0.5) for the spectrum, transfer function, meters and octave banding, and a ring
soak that counts overruns and the worst audio callback. The figures depend on
the machine, so run it rather than trusting a table.

## Layout

```
include/analyzer.h   the C ABI the apps compile against (hand-maintained)
src/
  base/     contract checks, saturating float-to-int cast, number text
  dsp/      FFT, windows, spectra, transfer function, MTW, delay, meters,
            octave bands, generator, deconvolution, impulse analysis, EQ.
            No I/O, no platform dependencies.
  cal/      Calibration chain: converter samples to absolute dB SPL.
  plot/     Display data reduction and axis transforms. Emits no pixels.
  model/    Measurements, store, file format, REW text export, settings, WAV.
  engine/   Lock-free ring, triple buffer, analysis thread, allocation trap.
  audio/    AudioBackend, offline backend, CoreAudio (macOS), RemoteIO (iOS).
  ffi/      The C ABI's implementation.
  cli/      Headless harness and benchmarks.
tests/      GoogleTest, mirroring src/; tests/golden holds recorded outputs.
third_party/  KissFFT, dr_wav, GoogleTest, copied in.
docs/       HANDOFF (the narrative), READING-ORDER, CPP-CONVENTIONS, REW-PARITY.
```

The thread topology is where the performance comes from:

```
audio thread          ring         analysis thread     triple buffer    UI thread
------------                       ---------------                      ---------
hard deadline    -->  [ring]  -->  heavy FFT work  -->  [snapshot] -->  draws
deinterleave                       no deadline,          newest wins     never
and return                         must keep up                          blocks
```

Decisions worth knowing about:

**The audio callback cannot allocate, and that is enforced rather than trusted.**
Every real-time audio codebase has this rule; most enforce it by code review,
which means violations ship and surface as a click once an hour on somebody
else's machine. Here the callback runs inside `rt_section()`, and the test
binaries replace the global `operator new` with one that aborts the process
inside it.

**The capture ring is interleaved and writes are all-or-nothing.** With one ring
per channel a partial write could advance one channel and not another, and
channels that drift by a single sample destroy a transfer-function phase reading.
Dropped blocks are counted and surfaced, never hidden - a measurement taken across
dropped audio is wrong, not merely noisy.

**The core emits no pixels.** Line traces are generated as geometry, but the
waterfall is one column of data per frame written into a GPU ring texture.
Recompositing a full Retina waterfall on the CPU is roughly what REW does, and it
is why its waterfall is slow.

**Axis mapping lives in the core and is queried, never reimplemented.** Cursor
readout, hit-testing and the drawn curve have to agree exactly, and a UI doing
its own bin-to-pixel arithmetic is how they quietly stop agreeing.

**Measurements are stored unsmoothed, complex, in double precision, with their
absolute references attached.** Smoothing is a view transform; storing a smoothed
magnitude curve forecloses group delay, RT60 and minimum-phase decomposition
forever. An unknown SPL offset stays an empty `std::optional` rather than
becoming zero, because "not measured" and "measured as needing no correction"
are different facts.

## Portability

macOS comes first and deep, but the deferral is designed for rather than assumed
away: no application logic lives in Swift, `audio::AudioBackend` and `dsp::Fft`
are abstract classes with one real implementation each, and no Apple SDK type
appears outside `src/audio/` (behind the guards in `audio/target.hpp`) and the
client repositories. Everything else, the C ABI included, builds and passes its
tests on Linux and Windows.

## Not done yet

The plan's own exit criterion for the headless core is a numeric match against
REW: ±0.1 dB on synthetic signals and ±0.5 dB on a real measurement, 20 Hz to
20 kHz. **That comparison has still not been run.** Internal consistency is
verified against analytically known answers throughout, which is a different and
weaker claim.

Everything needed to run it exists - `--generate`, `--compare`, and the
procedure in `docs/REW-PARITY.md`. What is missing is REW's own half: importing
each file and exporting its measurement, which is manual until REW's API is
used to automate it.

Also outstanding: scope view, group delay, minimum-phase decomposition, and the
Windows and Linux clients.

## A note on microphone permission

macOS gates capture behind TCC, and it will not raise a permission prompt for a
process launched in a non-interactive background session - it refuses outright,
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

The vendored libraries in `third_party/` are all permissive: KissFFT
(BSD-3-Clause), GoogleTest (BSD-3-Clause, tests only) and dr_wav (public domain
or MIT-0). See `third_party/README.md`. FFTW stays excluded on licence grounds.
