# The REW parity run

The project's one unmet commitment: match REW's exported magnitude to **±0.1 dB
on synthetic signals** and **±0.5 dB on a real measurement**, 20 Hz to 20 kHz.

This was Milestone 0's exit criterion and four milestones have been built past
it. If it turns up a systematic error — a window amplitude correction, an FFT
scaling factor, a calibration constant — that error sits underneath everything
since, including the equaliser and the optimiser fitted against it.

Everything needed is in the harness. What is not automated is REW itself.

## What you need

- REW installed. 5.19 is what this was written against.
- Its measurement export, as text.

## 1. Generate the test signals

```bash
mkdir -p /tmp/parity && cd /tmp/parity
A="cargo run --release -q -p analyzer-cli --"

# On-bin sine: isolates absolute scaling and the reference convention.
# 996.09375 Hz is bin 85 exactly at 4096 points and 48 kHz, so there is no
# scalloping loss to reason about.
$A --generate sine --hz 996.09375 --amplitude 0.5 --seconds 4 --out sine-onbin.wav

# Off-bin sine: isolates window shape and scalloping handling.
$A --generate sine --hz 1000 --amplitude 0.5 --seconds 4 --out sine-offbin.wav

# Noise: isolates per-bin level against power-per-hertz, and the window's
# noise bandwidth. This is where a normalisation mismatch shows up.
$A --generate pink  --amplitude 0.5 --seconds 8 --out pink.wav
$A --generate white --amplitude 0.5 --seconds 8 --out white.wav

# A sweep, which exercises the measurement path rather than the RTA path.
$A --generate sweep --hz 20 --hz-end 20000 --amplitude 0.5 --seconds 8 --out sweep.wav
```

Generation is deterministic — the same command produces the same bytes — so a
run can be repeated exactly, and a noise measurement can be compared against
itself.

Files are 32-bit float by default. Use `--depth i24` if REW will not read
float; do not use `i16` for this, because quantisation noise at −96 dBFS lands
in the same place as the differences being measured.

## 2. Analyse with this harness

```bash
$A pink.wav --fft 8192 --window hann --overlap 75 --average infinite > ours-pink.txt
```

Record the settings. They have to match what REW is told to do, and the header
of the output file records them for you.

## 3. Analyse the same file in REW

Import the WAV, set the RTA to the **same** FFT length, window, overlap and
averaging, and export the measurement as text.

Three settings decide whether this comparison means anything:

- **FFT length and window** must match exactly.
- **Averaging** must match, and must have settled. An unsettled average of
  noise differs from a settled one by more than the tolerance.
- **Per-bin level against spectral density.** A spectrum analyser can report
  "the level in this bin" or "power per hertz", and the two differ by
  10·log₁₀(ENBW) — about 16 dB for a flat-top at 4096 points. If the constant
  offset in the comparison comes out near a round number like that, this is
  why.

## 4. Compare

```bash
$A --compare ours-pink.txt rew-pink.txt --from-hz 20 --to-hz 20000
```

Add `--tolerance 0.1` to make it a gate: it exits non-zero when the shapes
disagree by more than that, so the whole run can be a script.

## Reading the result

The report gives two numbers and they mean different things.

**The constant offset** is a reference convention, not a defect. `0 dBFS =
full-scale sine` against `0 dBFS = full-scale square` is 3.01 dB. Per-bin level
against spectral density is the window's noise bandwidth. Neither is wrong, and
neither should fail the run.

**The deviation after that offset is removed** is the number the criterion
should be read against. That is a real disagreement in shape, and the likely
causes, in the order worth checking:

| Symptom | Likely cause |
|---|---|
| Deviation changes with window choice | Amplitude correction: coherent gain applied where ENBW belongs, or the reverse |
| Deviation grows toward Nyquist | A scaling factor applied per bin rather than per spectrum |
| Only the off-bin sine disagrees | Scalloping, or a peak-finding difference rather than a spectrum difference |
| Only noise disagrees, tones match | Per-bin against per-hertz normalisation |
| Deviation tracks the interpolated count | The comparison, not the analysers — match the FFT sizes |

Run it across at least Hann, flat-top and Blackman-Harris. A deviation that
appears with one window and not another is the strongest signal available about
where the fault is.

## The real-measurement half

±0.5 dB against a real measurement needs an actual captured sweep through a
speaker and a room, which needs a device that can play and record at once — an
Aggregate Device on most laptops. Capture it once with `--measure`, keep the
WAV short, and either keep it out of the repository or document how to
regenerate it. Do not commit a large binary for this.

## What is not automated

Driving REW. It has an API on `localhost:4735` (enable it in its preferences)
which could automate import and export and make the whole run a single script.
Until then step 3 is manual, and it is the only manual step.
