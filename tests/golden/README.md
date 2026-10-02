# Golden outputs from the Rust core

Reference outputs recorded from the Rust build (`analyzer-cli` and the model
crates) so the C++ port can be checked end to end after the Rust is deleted.

- **Regenerate:** only against the Rust core, which has been removed. The last
  revision containing it is `e9a459b`: check that out and run
  `bash tests/golden/generate.sh` (needs a Rust toolchain). On any later
  revision `fixtures/` is the record, and the C++ golden tests in
  `tests/model/golden_test.cpp` and `tests/cli/golden_test.cpp` hold the C++ to
  it.
- **Layout:** everything generated is under `fixtures/`; `generate.sh`,
  `rust/` (the fixture writer) and this file sit beside it.
- **Size:** about 2.2 MB.
- Recorded on Linux x86_64 with rustc 1.99. Re-running on another platform may
  move the last bit of a transcendental (`sin`/`exp`) sample; that is inside the
  tolerances below, but means "second run leaves `git status` clean" is a
  same-machine promise.

## Driving a test from `fixtures/MANIFEST.tsv`

`MANIFEST.tsv` is written by the same loop that runs the commands, so it is the
authoritative record of the exact command for every CLI fixture. Tab-separated,
first row is a header:

| column | meaning |
|---|---|
| `id` | case name; also the stem of its stdout/stderr files |
| `exit` | exit status the Rust CLI returned (`0` or `1`) |
| `class` | comparison class, defined below |
| `stdout` | expected stdout, relative to `fixtures/` (may be empty) |
| `stderr` | expected stderr, or `-` when it was empty |
| `artifact` | file the command itself writes (`--generate`, `--out`), or `-` |
| `args` | argument vector, single-space separated (no argument contains a space) |

To run a case: `cd tests/golden/fixtures`, run the C++ CLI with `args`
(**the working directory matters**: paths in args are relative to `fixtures/`,
and `--measure` echoes them into its report), capture stdout and stderr to
separate scratch files, then check exit status, stdout, stderr and artifact
under the rules below. Case inputs (`wav/*.wav`, and the `.txt` files fed to
`--compare`) are the **committed** files, never the C++ run's own output. Run
`--generate` cases into a scratch directory (keeping the `wav/...` argument
shape) so the committed inputs are not overwritten, or run them first and
compare. Anything not named below is exact.

Recreate by hand, for example:

    cd tests/golden/fixtures
    analyzer-cli --fft 1024 --window flattop --overlap 50 wav/pink_f32.wav
    # == stdout/rta_pink_f32_fft1024_flattop_ov50.txt

## Comparison classes

### `exact`
Stdout and stderr byte-identical, same exit status. Error messages are
`analyzer-cli: <message>`; the strings are the contract (the C++ must say the
same words). Exception: `help` should exit 0 and print a usage text; its words
legitimately differ if the C++ CLI drops `--live`, `--list-devices` or `--bench`,
so treat byte-equality there as optional.

### `wavgen` (`--generate`)
Stdout (`# wrote N frames ...`) is exact, as is the exit status. The written
file is `artifact`:

- **Header** (every byte before the sample payload) **byte-exact**. 16-bit files
  have the classic 44-byte PCM header. 24-bit and float files are
  `WAVE_FORMAT_EXTENSIBLE` with a 68-byte header (`fmt ` size 40, format tag
  `0xFFFE`, no `fact` chunk). Frame count, `RIFF` size and `data` size exact.
- **Payload**: `i16`/`i24` within **1 LSB** of the fixture; `f32` within
  **1e-6** absolute. White noise is integer arithmetic only and should come out
  bit-exact; sine and sweep use `sin`/`exp` in `f64` cast to `f32`; pink adds an
  `f32` filter chain (an FMA-contracting compiler will differ by an ulp).
- Things the C++ has to get right to land inside that: noise seed
  `0x5EED5EED5EED5EED` (xorshift64, multiplier `0x2545F4914F6CDD1D`, top 24
  bits); integer depths scale by `2^(bits-1) - 1` (32767, 8388607) and round
  half away from zero, while the reader divides by `2^(bits-1)` (32768,
  8388608), so full scale is not symmetric; amplitude default 0.5; sweep is a
  single non-repeating pass of exactly `--seconds`, then zeros.

### `spectrum` (WAV or `--sine` input)
- **Comment lines (`#...`)**: byte-exact, except the ENBW figure in the
  `# window:` line, which may differ by 0.0001 Hz. Note the window name is Rust
  `Debug` text, including `Tukey { alpha: 0.25 }`, and the averaging name is
  `Infinite` / `PeakHold` / `None`.
- **Rows**: same count (`--min-db` excepted: one row more or fewer at the
  threshold is fine). The frequency column is exact (6 decimals).
- **Level column** (dBFS, 4 decimals). The C++ uses a different FFT, so
  tolerance depends on how far below full scale the bin is, because float
  round-off in the transform sets a floor near -130 dBFS (the pure-sine cases
  reach -197 dBFS, which is noise):

  | bin level in the fixture | allowed difference |
  |---|---|
  | >= -90 dBFS | 0.01 dB |
  | -110 to -90 | 0.1 dB |
  | -130 to -110 | 1.0 dB |
  | < -130 | not compared |

  These bands come from reasoning about `f32` round-off, not from running a
  second FFT. Start here, and widen a band only with a written reason.
- `--peak` output is one row: frequency exact, level within 0.01 dB. The
  on-bin cases (`sine_onbin_peak_*`) must read -6.0206 within 0.01 for every
  window; that is the scaling check the Rust harness tests rely on.
- Invariant worth a direct assertion: `rta_pink_f32_block1000` must equal
  `rta_pink_f32_default` (callback block size cannot change the answer).
- `rta_out_flag`: stdout empty, stderr `wrote files/peak_out.txt`, and the file
  `files/peak_out.txt` is `artifact`, compared as a `spectrum`.

### `measure` (`--measure`, `--measure-demo`)
A text report of `#` lines then log-spaced table rows. Line count, labels and
layout are exact. Numbers:

| item | allowed difference |
|---|---|
| `direct arrival` ms (and metres) | 0.02 ms |
| `impulse length` samples | exact |
| `reflection +x ms at y%` | same number of lines; x 0.02 ms, y 2 points |
| `EDT` / `T20` / `T30` seconds | 0.02 s; `(insufficient range)` must match |
| `agreement` percentage | present and formatted alike; value not compared |
| `gated response:` window ms, valid-above Hz | exact |
| table `frequency_hz` | exact (1 decimal) |
| table `level_db` | 0.1 dB |
| table `trustworthy` | exact |

`measure_files_identity` deconvolves a signal against itself, a degenerate case
whose decay fits sit at about a millisecond, so there the RT and `agreement`
lines are format-only; arrival, length and the table are still checked. The
`constructed:` block in `--measure-demo` output is exact. The demo's room uses a
seeded white-noise generator (seed 99) and a seed-1 sweep, so it is
reproducible; its stdout is a pure function of the arguments.

### `compare` (`--compare`)
Inputs are committed text files, so the answer is a pure function of them and
can be tight. Layout and exit status exact. Deviation and offset figures within
0.0002 dB. The `at ... Hz` frequencies are exact, except in `compare_agree_pass`
and `compare_agree_band`, where the deviations are about 0.0001 dB and the
location of the maximum is a tie; ignore those two frequencies.
`compare_disagree_fail` writes the report plus `shapes differ by ...` to
**stderr** and nothing to stdout, exit 1. `compare_no_overlap` exits 0 and says
nothing overlapped.

## Fixtures from the CLI

All under `fixtures/`; command text is in `MANIFEST.tsv`.

| group | ids | notes |
|---|---|---|
| Generated audio | `gen_{sine,pink,white,sweep}_{i16,i24,f32}` | 0.5 s, 48 kHz, mono, default amplitude and frequencies; written to `wav/{kind}_{depth}.wav` |
| | `gen_sine440_44k1_i24`, `gen_sweep_100_5000_i16`, `gen_sweep_1s_f32`, `gen_sweep_44k1_i16` | non-default `--hz`, `--hz-end`, `--rate`, `--amplitude`; the 1 s sweep and the 44.1 kHz sweep feed `--measure` |
| | `gen_err_*` | bad signal name, missing `--out` |
| Spectrum of a file | `rta_*` | `--fft` 1024/2048/8192, windows `rect hann bh flattop tukey`, overlaps 0/50/87, averaging `none`/`peak`, `--min-db`, `--block`, `--out` |
| `--peak` | `peak_*` | |
| `--sine` | `sine_*` | on-bin 996.09375 Hz for every window; 440 Hz at 44.1 kHz |
| Errors | `err_*`, `help` | short source, unknown window/option, odd FFT, channel out of range, no input |
| Swept measurement | `measure_*` | demo twice (default, `--gate 10 --fft 2048`); real files: room, room with gate 10 / FFT 8192, identity, rate mismatch |
| Compare | `compare_*` | agreeing and disagreeing pairs, with and without `--tolerance` (exit 0 and 1), band limits, REW-style text input, no overlap, bad band |

`--live`, `--list-devices` and `--bench` have no fixtures: they need hardware or
report wall-clock timings.

## Fixtures the CLI cannot produce (`rust/`)

`rust/` is a separate cargo project (own `[workspace]`, own `Cargo.lock`, path
dependencies on `analyzer-dsp` and `analyzer-model`). `generate.sh` runs it as
`cargo run --release --locked --manifest-path tests/golden/rust/Cargo.toml -- fixtures`.
Every input is hard-coded and exactly representable. With
`ramp(i, m) = ((i * m) mod 64) / 64 - 0.5` (exact in binary), a C++ test can
rebuild each measurement and compare, or read the fixture and write it back.

| fixture | contents |
|---|---|
| `model/measurement_impulse_response.anlz` | id 7, name `Living room, left\nsecond line`, notes containing a backslash and newline, `captured_at` 1700000000, 48 kHz, 2 channels, 256 samples `ramp(i,37)`, time zero 12.5, all five references set |
| `model/measurement_spectrum.anlz` | id 8, 64 complex bins at 750 Hz, bin 0 is zero, bin k>0 is `(2*ramp(k,5), ramp(k,11))`, SPL offset 94 |
| `model/measurement_power_spectrum.anlz` | id 9, 44.1 kHz, 32 bins `-20 - 40*ramp(k,3)` dB at 1378.125 Hz, **no references** (absent lines, not zeros) |
| `model/measurement_transfer_function.anlz` | id 10, 32 bins `(1 + ramp(k,7), ramp(k,13))`, coherence `0.5 + (k mod 8)/16`, 93.75 Hz, delay 0.005 s, SPL offset **0** (present, unlike the power spectrum) |
| `model/measurement_*.rew.txt` | `export::to_text` of each of the four above |
| `model/filters_all_kinds.{rew,apo}.txt` | 10 bands (PK, LSC, HSC, HPQ, LPQ, BP, NO, AP, one disabled, one transparent PK at 0 dB), preamp -6.25 |
| `model/filters_all_kinds.minidsp_{48k,96k}.txt` | the same bands as miniDSP biquads at 48 and 96 kHz |
| `model/filters_empty.{rew,apo,minidsp_48k}.txt` | no bands, preamp -3 (miniDSP must still carry the trim as one gain-only section) |
| `model/settings_default.cfg`, `settings_custom.cfg` | `Settings::default()` and a fully custom set |
| `model/settings_hand_edited.in.cfg` / `.out.cfg` | a damaged file and what `Settings::from_text(...).to_text()` makes of it: unknown key dropped, bad values fall back per field, FFT 3000 to 4096, inverted axes repaired, `spl_offset_db: 0` kept, empty `mic_cal_path` dropped |
| `wav/response_room_f32.wav` | the `sweep_1s_f32` stimulus through four arrivals, `(delay samples, gain)` = (240, 0.5), (336, 0.3), (768, -0.2), (1440, 0.1), length `n + 1441`; input to `measure_files_*` |

Rules for these:

- **`.anlz`: byte-exact**, header and payload (little-endian `f64`; complex data
  as real, imaginary pairs; transfer function is bins then coherence). Header
  numbers are Rust `Display`: shortest round-trip decimal, never scientific
  notation (`0.0078125`, not `7.8125e-3`). `format::read` then `write` must give
  the same bytes.
- **`.rew.txt`**: header lines and the impulse-response export (`{:.9}`, pure
  arithmetic on exact values) byte-exact. In the spectrum, power-spectrum and
  transfer-function rows the frequency column is exact and the SPL, phase and
  coherence columns, which pass through `log10` and `atan2`, may differ by
  0.0001 in the last printed digit. The 0 Hz row of a spectrum prints
  `-200.0000` (zero magnitude is floored and the SPL offset is *not* added).
- **`filters_*.rew.txt`, `.apo.txt`: byte-exact.** Two formatting traps are
  recorded here on purpose: preamp -6.25 prints as `-6.2` (an exact binary tie
  rounds to even, as `printf("%.1f")` does), and APO prints `f32` values in
  shortest form (`63`, `0.707`, `-5.5`).
- **`filters_*.minidsp_*.txt`**: block structure and labels exact (`biquadN,`,
  `b0=`...`a2=`, no trailing comma on the last `a2`). Coefficients are the
  designed `f32` printed to 15 decimals, with `a1` and `a2` **negated**;
  compare as numbers within 1e-6 absolute (start tight, widen to 1e-5 only with
  a reason). Preamp is folded into the first section's `b0..b2`. In the empty
  case `a1`/`a2` print as `-0.000000000000000` (negated zero keeps its sign).
- **`settings_*.cfg`: byte-exact** (values are `f32` shortest form, `94.3`).
  `settings_hand_edited.in.cfg` is the input; the output is the contract for the
  per-field fallback and range repair.

## Determinism and exclusions

Nothing in a fixture depends on a clock, a random source or the machine:

- Noise uses a fixed seed (`wav::GENERATOR_SEED`); the offline analyser is
  single-threaded and reproducible by design; `--measure-demo` seeds its own
  generators.
- `captured_at` is a hard-coded field in the fixture writer, not a timestamp.
  The REW filter export deliberately has no date line.
- Report paths are relative because every case runs from `fixtures/`. The
  `--generate` stdout includes the `--out` path as given.
- Excluded because they are not reproducible: `--bench` (wall-clock timings),
  `--live` and `--list-devices` (hardware), and OS error text such as a missing
  input file (platform wording).
