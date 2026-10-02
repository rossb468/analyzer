#!/usr/bin/env bash
# Regenerate every golden fixture from the Rust core, from scratch.
#
#   bash tests/golden/generate.sh
#
# Output goes to tests/golden/fixtures/ (deleted first). Two runs on the same
# toolchain give byte-identical output: every signal uses a fixed seed
# (analyzer_model::wav::GENERATOR_SEED), no fixture records a clock time, and
# paths in tool output are always relative to fixtures/ because every case runs
# with that as its working directory.
#
# bash 3.2 compatible (macOS /bin/bash): no associative arrays, no mapfile.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../.." && pwd)
fx="$here/fixtures"
manifest="$fx/MANIFEST.tsv"
export LC_ALL=C

# --- Build ------------------------------------------------------------------
# Status is read directly from each command; nothing here is piped.
(cd "$root" && cargo build --release --locked -p analyzer-cli)
cli="${CARGO_TARGET_DIR:-$root/target}/release/analyzer-cli"
if [ ! -x "$cli" ]; then
    echo "generate.sh: expected CLI at $cli" >&2
    exit 1
fi

# --- Clean slate ------------------------------------------------------------
rm -rf "$fx"
mkdir -p "$fx/stdout" "$fx/stderr" "$fx/wav" "$fx/files" "$fx/model"

# --- Fixtures the CLI cannot produce ---------------------------------------
# Writes fx/model/* and fx/wav/response_room_f32.wav.
cargo run --quiet --release --locked --manifest-path "$here/rust/Cargo.toml" -- "$fx"

# --- CLI cases --------------------------------------------------------------
# MANIFEST.tsv columns, tab separated:
#   id  exit  class  stdout  stderr  artifact  args
# `args` is the argument vector, single-space separated (no argument contains a
# space), to be run with fixtures/ as the working directory. `stderr` and
# `artifact` are "-" when there is none. See README.md for the classes.
printf 'id\texit\tclass\tstdout\tstderr\tartifact\targs\n' >"$manifest"

# run_case <id> <class> <artifact|-> <args...>
run_case() {
    local id=$1 class=$2 artifact=$3
    shift 3
    local status=0
    # Redirect to files and capture the status directly; a failing case is
    # recorded, not fatal.
    (cd "$fx" && "$cli" "$@" >"stdout/$id.txt" 2>"stderr/$id.txt") || status=$?
    local err="stderr/$id.txt"
    if [ ! -s "$fx/$err" ]; then
        rm -f "$fx/$err"
        err="-"
    fi
    local args="$*"
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
        "$id" "$status" "$class" "stdout/$id.txt" "$err" "$artifact" "$args" >>"$manifest"
}

# 1. Signal generation: every kind at every depth, 0.5 s at 48 kHz.
for kind in sine pink white sweep; do
    for depth in i16 i24 f32; do
        run_case "gen_${kind}_${depth}" wavgen "wav/${kind}_${depth}.wav" \
            --generate "$kind" --depth "$depth" --seconds 0.5 --out "wav/${kind}_${depth}.wav"
    done
done
# Non-default frequency, rate, amplitude; the sweep range flags; a long sweep to
# measure with; a 44.1 kHz sweep to provoke a rate mismatch.
run_case gen_sine440_44k1_i24 wavgen wav/sine440_44k1_i24.wav \
    --generate sine --hz 440 --rate 44100 --amplitude 0.25 --depth i24 --seconds 0.5 \
    --out wav/sine440_44k1_i24.wav
run_case gen_sweep_100_5000_i16 wavgen wav/sweep_100_5000_i16.wav \
    --generate sweep --hz 100 --hz-end 5000 --depth i16 --seconds 0.5 \
    --out wav/sweep_100_5000_i16.wav
run_case gen_sweep_1s_f32 wavgen wav/sweep_1s_f32.wav \
    --generate sweep --seconds 1 --depth f32 --out wav/sweep_1s_f32.wav
run_case gen_sweep_44k1_i16 wavgen wav/sweep_44k1_i16.wav \
    --generate sweep --rate 44100 --seconds 0.25 --depth i16 --out wav/sweep_44k1_i16.wav
run_case gen_err_unknown_signal exact - --generate triangle --out wav/never.wav
run_case gen_err_no_out exact - --generate sine

# 2. Spectrum of a WAV file (the default mode).
run_case rta_pink_f32_default spectrum - wav/pink_f32.wav
run_case rta_pink_f32_block1000 spectrum - --block 1000 wav/pink_f32.wav
run_case rta_pink_f32_fft1024_flattop_ov50 spectrum - \
    --fft 1024 --window flattop --overlap 50 wav/pink_f32.wav
run_case rta_pink_i24_default spectrum - wav/pink_i24.wav
run_case rta_pink_i16_default spectrum - wav/pink_i16.wav
run_case rta_pink_i24_tukey_ov0_peakhold spectrum - \
    --fft 2048 --window tukey --overlap 0 --average peak wav/pink_i24.wav
run_case rta_white_f32_default spectrum - wav/white_f32.wav
run_case rta_white_i16_fft2048_bh_avgnone spectrum - \
    --fft 2048 --window bh --average none wav/white_i16.wav
run_case rta_sweep_f32_fft8192_rect_ov87 spectrum - \
    --fft 8192 --window rect --overlap 87 wav/sweep_f32.wav
run_case rta_sine_f32_min90 spectrum - --min-db -90 wav/sine_f32.wav
run_case rta_sine_i16_fft2048_flattop spectrum - --fft 2048 --window flattop wav/sine_i16.wav
# --peak: one row, the loudest bin.
run_case peak_sine_i16_hann spectrum - --peak wav/sine_i16.wav
run_case peak_sine_i24_rect spectrum - --peak --window rect wav/sine_i24.wav
run_case peak_sine_f32_fft8192_flattop spectrum - --peak --fft 8192 --window flattop wav/sine_f32.wav
run_case peak_sine440_44k1_bh spectrum - --peak --window bh wav/sine440_44k1_i24.wav
# --out writes the report to a file and says so on stderr.
run_case rta_out_flag spectrum files/peak_out.txt --peak --out files/peak_out.txt wav/sine_f32.wav

# 3. Synthetic sine (--sine): no file involved.
run_case sine_1000_default spectrum - --sine 1000
run_case sine_440_44k1_bh_fft2048 spectrum - \
    --sine 440 --rate 44100 --seconds 0.5 --amplitude 0.25 --window bh --fft 2048
for w in rect hann bh flattop tukey; do
    # 996.09375 Hz is exactly bin 85 at 48 kHz / 4096: every window reads -6.02.
    run_case "sine_onbin_peak_${w}" spectrum - --sine 996.09375 --peak --window "$w"
done

# 4. Argument errors and short sources (stderr + exit status only).
run_case err_short_source exact - --sine 1000 --seconds 0.05
run_case err_unknown_window exact - --sine 1000 --window nope
run_case err_odd_fft exact - --sine 1000 --fft 1001
run_case err_channel_out_of_range exact - --sine 1000 --channel 1
run_case err_unknown_option exact - --frobnicate
run_case err_no_input exact -
run_case help exact - --help

# 5. Swept measurement.
run_case measure_demo_default measure - --measure-demo
run_case measure_demo_gate10_fft2048 measure - --measure-demo --gate 10 --fft 2048
run_case measure_files_room measure - --measure wav/sweep_1s_f32.wav wav/response_room_f32.wav
run_case measure_files_room_gate10_fft8192 measure - \
    --measure wav/sweep_1s_f32.wav wav/response_room_f32.wav --gate 10 --fft 8192
run_case measure_files_identity measure - --measure wav/sweep_1s_f32.wav wav/sweep_1s_f32.wav
run_case measure_err_rate_mismatch exact - --measure wav/sweep_1s_f32.wav wav/sweep_44k1_i16.wav

# 6. Compare. Inputs are fixtures written above, so the C++ run must use the
# committed files here, not its own freshly produced spectra.
run_case compare_agree_pass compare - \
    --compare stdout/rta_pink_f32_default.txt stdout/rta_pink_i24_default.txt --tolerance 0.1
run_case compare_agree_band compare - \
    --compare stdout/rta_pink_f32_default.txt stdout/rta_pink_i16_default.txt \
    --from-hz 100 --to-hz 10000
run_case compare_disagree_readout compare - \
    --compare stdout/rta_pink_f32_default.txt stdout/rta_white_f32_default.txt
run_case compare_disagree_fail compare - \
    --compare stdout/rta_pink_f32_default.txt stdout/rta_white_f32_default.txt --tolerance 1
run_case compare_rew_export_self compare - \
    --compare model/measurement_spectrum.rew.txt model/measurement_spectrum.rew.txt --tolerance 0.001
run_case compare_rew_export_cross compare - \
    --compare model/measurement_spectrum.rew.txt model/measurement_power_spectrum.rew.txt
run_case compare_no_overlap compare - \
    --compare stdout/rta_pink_f32_default.txt model/measurement_spectrum.rew.txt \
    --from-hz 30000 --to-hz 40000
run_case compare_err_bad_band exact - \
    --compare stdout/rta_pink_f32_default.txt stdout/rta_pink_i24_default.txt --from-hz 500 --to-hz 100

echo "golden fixtures written to $fx"
