#ifndef ANALYZER_H
#define ANALYZER_H

/*
 * analyzer.h - the C ABI of the analyzer core.
 *
 * THIS FILE IS HAND-MAINTAINED. It is the source of truth for the C ABI, and
 * the implementation in src/ffi/ is written to match it.
 *
 * It is the ABI contract with the macOS and iOS apps, which compile Swift
 * against this exact text. Anything that changes it - a type, a field, an
 * enumerator value, a signature - must change together with both apps, in the
 * same pass, and the apps' CI is what catches a mismatch. Do not reorder
 * struct fields, renumber enumerators or change a type "while you are there".
 *
 * Two rules the layout depends on:
 *
 *  - Plain C, no C++ guards. With cpp_compat off, an enum is declared once as
 *    `enum X : uint32_t` under C23 and as `typedef uint32_t X` before it.
 *    Swift imports C with -std=c23; an enum that appears twice makes the
 *    importer see two candidates for one name.
 *  - Everything is POD, or opaque behind a pointer. Nothing here allocates
 *    memory the caller has to free except the explicit `_destroy` and `_stop`
 *    functions.
 *
 * tests/ffi/c_compile_test.c compiles it as C11 and C23.
 */


#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

/**
 * Longest message `AnalyzerStatus` can carry, including the terminator.
 */
#define ANALYZER_MESSAGE_LEN 256

/**
 * Most bands an equaliser can carry across the boundary.
 *
 * Fixed so the coefficients handed to the audio thread are a plain array with
 * no allocation behind them. Twenty-four is more than any room correction
 * needs and more than any hardware unit this would be exported to accepts.
 */
#define ANALYZER_MAX_EQ_BANDS 24

/**
 * Harmonic orders reported across the boundary.
 *
 * A fixed array keeps the struct POD with nothing for the caller to free. Ten
 * is what an audio measurement conventionally covers, and anything beyond it is
 * below the noise floor of any real system.
 */
#define ANALYZER_MAX_HARMONICS 10

/**
 * Longest trace name carried across the boundary, including the terminator.
 */
#define ANALYZER_TRACE_NAME_LEN 128

/**
 * Longest path `AnalyzerSettings` can carry, including the terminator.
 */
#define ANALYZER_PATH_LEN 1024

/**
 * Analysis window, mirroring `WindowKind`.
 */
enum AnalyzerWindow
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * No taper.
   */
  AnalyzerWindow_Rectangular = 0,
  /**
   * General-purpose default.
   */
  AnalyzerWindow_Hann = 1,
  /**
   * Low leakage, wider main lobe.
   */
  AnalyzerWindow_BlackmanHarris = 2,
  /**
   * Flat main lobe, accurate amplitude. For calibration.
   */
  AnalyzerWindow_FlatTop = 3,
  /**
   * Tapered cosine at alpha 0.25.
   */
  AnalyzerWindow_Tukey = 4,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerWindow AnalyzerWindow;
#else
typedef uint32_t AnalyzerWindow;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Frame overlap, mirroring `Overlap`.
 */
enum AnalyzerOverlap
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * No overlap.
   */
  AnalyzerOverlap_None = 0,
  /**
   * 50%.
   */
  AnalyzerOverlap_Half = 1,
  /**
   * 75%.
   */
  AnalyzerOverlap_ThreeQuarters = 2,
  /**
   * 87.5%.
   */
  AnalyzerOverlap_SevenEighths = 3,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerOverlap AnalyzerOverlap;
#else
typedef uint32_t AnalyzerOverlap;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Averaging mode, mirroring the useful subset of `Averaging`.
 */
enum AnalyzerAveraging
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * Each frame replaces the last.
   */
  AnalyzerAveraging_None = 0,
  /**
   * Exponential with a one second time constant.
   */
  AnalyzerAveraging_Fast = 1,
  /**
   * Average everything since start.
   */
  AnalyzerAveraging_Infinite = 2,
  /**
   * Hold the maximum per bin.
   */
  AnalyzerAveraging_PeakHold = 3,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerAveraging AnalyzerAveraging;
#else
typedef uint32_t AnalyzerAveraging;
#endif // __STDC_VERSION__ >= 202311L

/**
 * What the session computes.
 */
enum AnalyzerMode
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * Single-channel spectrum.
   */
  AnalyzerMode_Spectrum = 0,
  /**
   * Two-channel transfer function, alongside the spectrum.
   */
  AnalyzerMode_Transfer = 1,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerMode AnalyzerMode;
#else
typedef uint32_t AnalyzerMode;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Where the transfer function's reference comes from.
 */
enum AnalyzerReference
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * The generator's own samples, captured alongside the input.
   *
   * Needs no loopback cable and works with a one-channel microphone, which
   * is what makes a transfer function possible on a bare laptop. The cost is
   * that it measures the acoustic path plus the converter round trip rather
   * than the acoustic path alone, so the delay finder has to remove a delay
   * it cannot know in advance.
   */
  AnalyzerReference_Internal = 0,
  /**
   * A second input channel, fed from a physical loopback.
   *
   * More accurate: the converter's own latency and response appear in both
   * channels and divide out.
   */
  AnalyzerReference_Input = 1,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerReference AnalyzerReference;
#else
typedef uint32_t AnalyzerReference;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Stimulus the generator produces.
 */
enum AnalyzerSignal
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * Generator off.
   */
  AnalyzerSignal_Silence = 0,
  /**
   * Steady sine, for distortion and calibration.
   */
  AnalyzerSignal_Sine = 1,
  /**
   * Equal energy per hertz.
   */
  AnalyzerSignal_WhiteNoise = 2,
  /**
   * Equal energy per octave. The usual transfer function stimulus.
   */
  AnalyzerSignal_PinkNoise = 3,
  /**
   * Exponential sine sweep, one pass. The swept-measurement stimulus.
   *
   * Not settable through `analyzer_session_set_signal`, which has nowhere
   * to put a sweep's extra parameters; it is armed by
   * `analyzer_session_start_measurement`.
   */
  AnalyzerSignal_Sweep = 4,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerSignal AnalyzerSignal;
#else
typedef uint32_t AnalyzerSignal;
#endif // __STDC_VERSION__ >= 202311L

/**
 * How several bins in one pixel column combine.
 */
enum AnalyzerReduction
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * Loudest bin. Preserves narrow peaks.
   */
  AnalyzerReduction_Max = 0,
  /**
   * Power-domain mean.
   */
  AnalyzerReduction_Mean = 1,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerReduction AnalyzerReduction;
#else
typedef uint32_t AnalyzerReduction;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Which transfer function curve a copy refers to.
 */
enum AnalyzerCurve
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * Magnitude in decibels. Maps through the ordinary level axis.
   */
  AnalyzerCurve_Magnitude = 0,
  /**
   * Phase in degrees, wrapped to -180..180. Maps through
   * `analyzer_phase_to_y`.
   */
  AnalyzerCurve_Phase = 1,
  /**
   * Coherence, 0..1. Maps through `analyzer_coherence_to_y`.
   */
  AnalyzerCurve_Coherence = 2,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerCurve AnalyzerCurve;
#else
typedef uint32_t AnalyzerCurve;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Which equaliser is active.
 */
enum AnalyzerEqMode
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * No equalisation. The stimulus is unfiltered and no curve is drawn.
   */
  AnalyzerEqMode_Off = 0,
  /**
   * Ten fixed octave bands, gains only.
   */
  AnalyzerEqMode_Graphic = 1,
  /**
   * Arbitrary bands of any shape.
   */
  AnalyzerEqMode_Parametric = 2,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerEqMode AnalyzerEqMode;
#else
typedef uint32_t AnalyzerEqMode;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Shape of an equaliser band.
 */
enum AnalyzerFilterKind
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * A bump or dip centred on the frequency.
   */
  AnalyzerFilterKind_Peaking = 0,
  AnalyzerFilterKind_LowShelf = 1,
  AnalyzerFilterKind_HighShelf = 2,
  AnalyzerFilterKind_LowPass = 3,
  AnalyzerFilterKind_HighPass = 4,
  AnalyzerFilterKind_BandPass = 5,
  AnalyzerFilterKind_Notch = 6,
  AnalyzerFilterKind_AllPass = 7,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerFilterKind AnalyzerFilterKind;
#else
typedef uint32_t AnalyzerFilterKind;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Which target shape is selected.
 */
enum AnalyzerTargetShape
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * Flat at every frequency.
   */
  AnalyzerTargetShape_Flat = 0,
  /**
   * A constant slope in decibels per octave.
   */
  AnalyzerTargetShape_Tilt = 1,
  /**
   * A bass shelf with an optional tilt above it.
   */
  AnalyzerTargetShape_Room = 2,
  /**
   * Points loaded from a file.
   */
  AnalyzerTargetShape_Custom = 3,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerTargetShape AnalyzerTargetShape;
#else
typedef uint32_t AnalyzerTargetShape;
#endif // __STDC_VERSION__ >= 202311L

/**
 * A format the equaliser can be written as.
 */
enum AnalyzerFilterFormat
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * REW's own filter settings text.
   */
  AnalyzerFilterFormat_Rew = 0,
  /**
   * An Equalizer APO configuration.
   */
  AnalyzerFilterFormat_EqualizerApo = 1,
  /**
   * miniDSP biquad coefficients.
   */
  AnalyzerFilterFormat_MiniDsp = 2,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerFilterFormat AnalyzerFilterFormat;
#else
typedef uint32_t AnalyzerFilterFormat;
#endif // __STDC_VERSION__ >= 202311L

/**
 * Sample format for a written file.
 */
enum AnalyzerSampleDepth
#if __STDC_VERSION__ >= 202311L
  : uint32_t
#endif // __STDC_VERSION__ >= 202311L
 {
  /**
   * 16-bit integer.
   */
  AnalyzerSampleDepth_Int16 = 0,
  /**
   * 24-bit integer.
   */
  AnalyzerSampleDepth_Int24 = 1,
  /**
   * 32-bit float. The default, because a generated signal has no reason to
   * be quantised.
   */
  AnalyzerSampleDepth_Float32 = 2,
};
#if __STDC_VERSION__ >= 202311L
typedef enum AnalyzerSampleDepth AnalyzerSampleDepth;
#else
typedef uint32_t AnalyzerSampleDepth;
#endif // __STDC_VERSION__ >= 202311L

/**
 * A snapshot of the devices present when it was created.
 *
 * Owning the strings in a list, rather than returning them one at a time, is
 * what makes the borrowed `const char*` in `AnalyzerDevice` safe: they stay
 * valid until the list is destroyed.
 */
typedef struct AnalyzerDeviceList AnalyzerDeviceList;

/**
 * A running capture and analysis session.
 *
 * Opaque across the boundary: C sees an incomplete type, so it only ever holds
 * a pointer.
 */
typedef struct AnalyzerSession AnalyzerSession;

/**
 * Captured curves, held independently of any session.
 */
typedef struct AnalyzerTraceStore AnalyzerTraceStore;

/**
 * One device, with borrowed strings valid while its list lives.
 */
typedef struct AnalyzerDevice {
  /**
   * Stable identifier to pass back when starting a session.
   */
  const char *uid;
  /**
   * Human-readable name.
   */
  const char *name;
  /**
   * Capture channels.
   */
  uint32_t input_channels;
  /**
   * Playback channels.
   */
  uint32_t output_channels;
  /**
   * Current rate in hertz.
   */
  double sample_rate;
  /**
   * Whether the system considers this the default capture device.
   */
  bool is_default_input;
} AnalyzerDevice;

/**
 * Everything needed to start capturing and analysing.
 */
typedef struct AnalyzerSessionConfig {
  /**
   * Device UID, or null for the system default input.
   */
  const char *device_uid;
  /**
   * Which device channel to analyse.
   */
  uint32_t channel;
  /**
   * FFT size; must be even and at least two.
   */
  uint32_t fft_size;
  /**
   * Requested callback size in frames.
   */
  uint32_t buffer_frames;
  /**
   * Analysis window.
   */
  AnalyzerWindow window;
  /**
   * Frame overlap.
   */
  AnalyzerOverlap overlap;
  /**
   * Averaging mode.
   */
  AnalyzerAveraging averaging;
  /**
   * What to compute.
   */
  AnalyzerMode mode;
  /**
   * Where the transfer function reference comes from.
   */
  AnalyzerReference reference;
  /**
   * Reference input channel, used when `reference` is
   * `AnalyzerReference::Input`.
   */
  uint32_t reference_channel;
  /**
   * Stimulus to play. `AnalyzerSignal::Silence` opens no output at all.
   */
  AnalyzerSignal signal;
  /**
   * Stimulus level in dBFS, as a peak amplitude. Clamped to at most 0.
   */
  float signal_level_db;
  /**
   * Sine frequency, used when `signal` is `AnalyzerSignal::Sine`.
   */
  float signal_hz;
  /**
   * Bit per device output channel; bit 0 is channel 0.
   */
  uint32_t output_mask;
} AnalyzerSessionConfig;

/**
 * Outcome of a call that can fail.
 *
 * The message is an inline fixed buffer rather than a pointer, so there is
 * nothing for the caller to free and no lifetime to reason about.
 */
typedef struct AnalyzerStatus {
  /**
   * Zero on success, non-zero on failure.
   */
  int32_t code;
  /**
   * NUL-terminated UTF-8 explanation. Empty on success.
   */
  char message[ANALYZER_MESSAGE_LEN];
} AnalyzerStatus;

/**
 * State of the transfer function.
 */
typedef struct AnalyzerTransferInfo {
  /**
   * Frames folded into the estimate. Zero means no transfer function is
   * running, or none has been produced yet.
   *
   * Coherence is identically one for a single frame, so a UI should not
   * present it as meaningful until this is comfortably above one.
   */
  uint32_t frames;
  /**
   * Delay currently removed from the reference, in samples.
   */
  uint32_t delay_frames;
  /**
   * The same delay in milliseconds.
   */
  float delay_ms;
  /**
   * Distance that delay corresponds to in air, in metres.
   */
  float delay_metres;
  /**
   * Whether a delay estimate has been asked for and not yet settled.
   */
  bool estimating;
} AnalyzerTransferInfo;

/**
 * One equaliser band.
 */
typedef struct AnalyzerBand {
  AnalyzerFilterKind kind;
  /**
   * Centre or corner frequency in hertz.
   */
  float hz;
  /**
   * Gain in decibels. Ignored by the pass and reject shapes.
   */
  float gain_db;
  /**
   * Quality factor. Higher is narrower.
   */
  float q;
  /**
   * Whether the band contributes. A disabled band keeps its settings.
   */
  bool enabled;
} AnalyzerBand;

/**
 * Headroom figures for the active equaliser.
 */
typedef struct AnalyzerEqInfo {
  /**
   * Bands the active equaliser holds.
   */
  uintptr_t band_count;
  /**
   * Largest gain applied anywhere in the audio band, in decibels.
   *
   * Bands add, so this can far exceed any single band's gain. A UI showing
   * it next to a trim control is the difference between an equaliser that is
   * safe to use and one that clips without saying so.
   */
  float peak_gain_db;
  /**
   * Output trim, in decibels.
   */
  float preamp_db;
  /**
   * Whether an equaliser is active at all.
   */
  bool active;
} AnalyzerEqInfo;

/**
 * Metadata about the most recent frame.
 */
typedef struct AnalyzerFrameInfo {
  /**
   * Increments once per published frame.
   */
  uint64_t sequence;
  /**
   * Blocks the audio callback had to drop. Non-zero invalidates the
   * measurement, and a UI is expected to say so rather than hide it.
   */
  uint64_t overruns;
  /**
   * Frames folded into the current average.
   */
  uint32_t frames_averaged;
  /**
   * Frames folded into the long-term average trace, which is what makes it
   * trustworthy. A UI can report this rather than presenting a curve built
   * from three frames as though it were settled.
   */
  uint32_t average_frames;
  /**
   * Rate the analysis ran at.
   */
  float sample_rate;
  /**
   * Hertz between adjacent bins.
   */
  float bin_spacing_hz;
} AnalyzerFrameInfo;

/**
 * A distortion measurement.
 */
typedef struct AnalyzerDistortion {
  /**
   * Fundamental frequency found in the spectrum.
   */
  float fundamental_hz;
  /**
   * Its level.
   */
  float fundamental_db;
  /**
   * Total harmonic distortion as a percentage of the fundamental.
   */
  float thd_percent;
  /**
   * The same figure in decibels.
   */
  float thd_db;
  /**
   * Distortion plus noise: everything that is not the fundamental.
   */
  float thd_n_percent;
  /**
   * Median level of the bins that are neither fundamental nor harmonic.
   */
  float noise_floor_db;
  /**
   * How many entries of the harmonic arrays are populated.
   */
  uint32_t harmonic_count;
  /**
   * Orders that fall above Nyquist and were therefore not measured. Non-zero
   * means the THD figure covers fewer orders than the full set.
   */
  uint32_t orders_above_nyquist;
  /**
   * Frequency of each harmonic found.
   */
  float harmonic_hz[ANALYZER_MAX_HARMONICS];
  /**
   * Each harmonic as a percentage of the fundamental.
   */
  float harmonic_percent[ANALYZER_MAX_HARMONICS];
  /**
   * Each harmonic relative to the fundamental, in decibels.
   */
  float harmonic_relative_db[ANALYZER_MAX_HARMONICS];
} AnalyzerDistortion;

/**
 * A gridline.
 */
typedef struct AnalyzerTick {
  /**
   * Value in hertz or decibels.
   */
  float value;
  /**
   * Pixel position along the axis.
   */
  float position;
  /**
   * Whether it deserves a label.
   */
  bool major;
} AnalyzerTick;

/**
 * A target curve, as a flat POD struct.
 *
 * The shape parameters are all carried regardless of which shape is selected,
 * so switching between them and back does not lose what was set.
 */
typedef struct AnalyzerTarget {
  /**
   * Which shape is evaluated.
   */
  AnalyzerTargetShape shape;
  /**
   * Lift at the bottom of the band, for `AnalyzerTargetShape::Room`.
   */
  float shelf_db;
  /**
   * Where the shelf reaches half its lift, in hertz.
   */
  float transition_hz;
  /**
   * Slope in decibels per octave, zero at 1 kHz.
   */
  float db_per_octave;
  /**
   * Alignment offset currently applied.
   */
  float offset_db;
  /**
   * Whether a custom curve has been loaded and has points.
   */
  bool has_custom;
} AnalyzerTarget;

/**
 * How a swept measurement is taken.
 */
typedef struct AnalyzerMeasureConfig {
  /**
   * Lowest frequency of the sweep, in hertz.
   */
  float start_hz;
  /**
   * Highest frequency of the sweep, in hertz.
   */
  float end_hz;
  /**
   * Sweep duration in seconds.
   */
  float seconds;
  /**
   * Sweep level in dBFS.
   */
  float level_db;
  /**
   * How long to keep recording after the sweep ends, for the decay.
   */
  float tail_seconds;
  /**
   * Gate length for the quasi-anechoic response, in milliseconds.
   */
  float gate_ms;
  /**
   * Transform size for the gated response.
   */
  uint32_t fft_size;
} AnalyzerMeasureConfig;

/**
 * How far a running measurement has got.
 */
typedef struct AnalyzerMeasureProgress {
  /**
   * Whether a sweep is running.
   */
  bool active;
  /**
   * Samples captured so far.
   */
  uintptr_t captured;
  /**
   * Samples the capture is waiting for.
   */
  uintptr_t total;
  /**
   * Whether the capture is full and ready to finish.
   */
  bool complete;
} AnalyzerMeasureProgress;

/**
 * What a completed measurement found.
 */
typedef struct AnalyzerMeasureResult {
  /**
   * Time of the impulse peak, in milliseconds.
   *
   * This includes the converter round trip, not just the flight time
   * through the air: the sweep is armed and the recording started as two
   * separate operations, and nothing synchronises them to a sample. It is
   * reported rather than corrected because the correction is a loopback
   * reference, which is a measurement in its own right.
   */
  float arrival_ms;
  /**
   * The same as a distance, on the same caveat.
   */
  float arrival_metres;
  /**
   * Largest absolute value in the impulse response.
   */
  float peak_amplitude;
  /**
   * Early decay time, in seconds.
   */
  float edt;
  /**
   * Whether the early decay time could be measured at all.
   */
  bool has_edt;
  /**
   * T20, in seconds.
   */
  float t20;
  /**
   * Whether T20 could be measured.
   */
  bool has_t20;
  /**
   * T30, in seconds.
   */
  float t30;
  /**
   * Whether T30 could be measured.
   */
  bool has_t30;
  /**
   * How far the decay estimates disagree, as a fraction of the largest.
   * Above roughly 0.1 the decay is not a straight line and no single number
   * describes it.
   */
  float decay_spread;
  /**
   * Whether the spread is meaningful, which needs at least two estimates.
   */
  bool has_decay_spread;
  /**
   * Finest frequency the gate can resolve.
   */
  float resolution_hz;
  /**
   * Points in the gated response.
   */
  uintptr_t points;
} AnalyzerMeasureResult;

/**
 * A captured trace, as the UI sees it.
 */
typedef struct AnalyzerTraceInfo {
  /**
   * NUL-terminated name.
   */
  char name[ANALYZER_TRACE_NAME_LEN];
  /**
   * Whether it is drawn.
   */
  bool visible;
  /**
   * Palette index chosen when it was captured.
   */
  uint32_t colour;
  /**
   * Points stored, at analysis resolution.
   */
  uintptr_t points;
  /**
   * Rate it was captured at.
   */
  float sample_rate;
  /**
   * Spacing between stored bins, in hertz.
   */
  float bin_spacing_hz;
} AnalyzerTraceInfo;

/**
 * How the automatic fit is constrained.
 */
typedef struct AnalyzerOptimiserConfig {
  /**
   * Most filters to produce.
   */
  uint32_t max_filters;
  /**
   * Low end of the corrected band, in hertz.
   */
  float from_hz;
  /**
   * High end of the corrected band, in hertz.
   */
  float to_hz;
  /**
   * Largest boost any one filter may apply.
   *
   * Deliberately much smaller than the cut limit by default: a dip in a room
   * measurement is usually a cancellation, and boosting one burns headroom
   * without filling it in.
   */
  float max_boost_db;
  /**
   * Largest cut any one filter may apply.
   */
  float max_cut_db;
  /**
   * Widest filter allowed.
   */
  float min_q;
  /**
   * Narrowest filter allowed.
   */
  float max_q;
  /**
   * Errors smaller than this are left alone.
   */
  float threshold_db;
} AnalyzerOptimiserConfig;

/**
 * What a fit produced.
 */
typedef struct AnalyzerOptimisation {
  /**
   * Filters placed.
   */
  uint32_t band_count;
  /**
   * RMS error across the corrected band before any filter.
   */
  float initial_error_db;
  /**
   * RMS error after every filter.
   */
  float final_error_db;
} AnalyzerOptimisation;

/**
 * Program settings, as a flat POD struct.
 *
 * The optional SPL offset is split into a flag and a value rather than using a
 * sentinel, because every sentinel worth choosing is a level someone could
 * legitimately measure.
 */
typedef struct AnalyzerSettings {
  /**
   * Transform size a new session starts with.
   */
  uint32_t fft_size;
  /**
   * Window a new session starts with.
   */
  AnalyzerWindow window;
  /**
   * Averaging a new session starts with.
   */
  AnalyzerAveraging averaging;
  /**
   * Whether to start capturing as soon as the window opens.
   */
  bool start_on_launch;
  /**
   * Low end of the frequency axis, in hertz.
   */
  float min_hz;
  /**
   * High end of the frequency axis, in hertz.
   */
  float max_hz;
  /**
   * Bottom of the level axis, in decibels.
   */
  float min_db;
  /**
   * Top of the level axis, in decibels.
   */
  float max_db;
  /**
   * Spacing of the horizontal gridlines, in decibels.
   */
  float level_grid_step;
  /**
   * Whether an SPL calibration has ever been measured.
   */
  bool has_spl_offset;
  /**
   * Offset from dBFS to dB SPL. Meaningless unless `has_spl_offset`.
   */
  float spl_offset_db;
  /**
   * NUL-terminated path to a microphone correction file. Empty for none.
   */
  char mic_cal_path[ANALYZER_PATH_LEN];
} AnalyzerSettings;

/**
 * Enumerate audio devices. Returns null only if enumeration panicked.
 */
struct AnalyzerDeviceList *analyzer_device_list_create(void);

/**
 * Release a device list.
 *
 * # Safety
 *
 * `list` must come from `analyzer_device_list_create` and not already be
 * destroyed.
 */
void analyzer_device_list_destroy(struct AnalyzerDeviceList *list);

/**
 * How many devices the list holds.
 *
 * # Safety
 *
 * `list` must be null or a live device list.
 */
uintptr_t analyzer_device_list_count(const struct AnalyzerDeviceList *list);

/**
 * Read one device. Returns false for a bad index.
 *
 * The strings in `out` point into the list and stay valid until it is
 * destroyed.
 *
 * # Safety
 *
 * `list` must be null or live; `out` must be null or writable.
 */
bool analyzer_device_list_get(const struct AnalyzerDeviceList *list,
                              uintptr_t index,
                              struct AnalyzerDevice *out);

/**
 * A configuration filled with the defaults a UI should start from.
 */
struct AnalyzerSessionConfig analyzer_session_config_default(void);

/**
 * Start capturing and analysing. Returns null on failure, with `status`
 * describing why.
 *
 * # Safety
 *
 * `config` must point to a valid configuration; its `device_uid`, if non-null,
 * must be a NUL-terminated string. `status` must be null or writable.
 */
struct AnalyzerSession *analyzer_session_start(const struct AnalyzerSessionConfig *config,
                                               struct AnalyzerStatus *status);

/**
 * Stop and release a session. Safe to call with null.
 *
 * # Safety
 *
 * `session` must come from `analyzer_session_start` and not already be
 * destroyed.
 */
void analyzer_session_stop(struct AnalyzerSession *session);

/**
 * Set the plot geometry. Call on resize, on zoom, or when the reduction
 * changes. Returns false for degenerate geometry.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_set_plot(struct AnalyzerSession *session,
                               float width_px,
                               float height_px,
                               float min_hz,
                               float max_hz,
                               float min_db,
                               float max_db,
                               AnalyzerReduction reduction);

/**
 * Whether a frame has arrived since the last `analyzer_session_copy_trace`.
 *
 * A UI can skip a redraw entirely when this is false, which is how idle CPU
 * stays near zero — a timer redrawing identical data is the real battery cost.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_has_new_frame(const struct AnalyzerSession *session);

/**
 * Copy the reduced trace into `out`, returning how many values were written.
 *
 * One value per pixel column, in decibels, at most `capacity` of them.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or point to at least
 * `capacity` writable floats.
 */
uintptr_t analyzer_session_copy_trace(struct AnalyzerSession *session,
                                      float *out,
                                      uintptr_t capacity);

/**
 * Copy one transfer function curve, one value per pixel column.
 *
 * Returns zero in spectrum mode, so a UI can call this unconditionally and
 * simply draw nothing.
 *
 * Each curve is reduced in the domain it actually lives in. Magnitude is
 * decibels and goes through the session's chosen reduction. Coherence takes
 * the worst value in a column, because showing the best would hide the
 * dropouts a user is looking for. Phase is averaged as a direction, so a
 * column straddling the wrap reads 180 rather than 0.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or point to at least
 * `capacity` writable floats.
 */
uintptr_t analyzer_session_copy_transfer(struct AnalyzerSession *session,
                                         AnalyzerCurve curve,
                                         float *out,
                                         uintptr_t capacity);

/**
 * Read the transfer function's state.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or writable.
 */
bool analyzer_session_transfer_info(struct AnalyzerSession *session,
                                    struct AnalyzerTransferInfo *out);

/**
 * Ask the core to measure the reference-to-measurement delay and remove it.
 *
 * Returns immediately. The estimate needs signal to work with, so it settles
 * over the next fraction of a second and appears in
 * `analyzer_session_transfer_info`; asking during silence waits rather than
 * answering zero.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_estimate_delay(struct AnalyzerSession *session);

/**
 * Set the reference delay by hand, in samples.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_set_delay(struct AnalyzerSession *session, uint32_t frames);

/**
 * Change the stimulus while running.
 *
 * Only takes effect if the session was started with a signal: opening an
 * output stream is a device operation and cannot happen from here. A session
 * started silent stays silent, which is why a UI offering a generator should
 * start one even when the initial choice is silence.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_set_signal(struct AnalyzerSession *session,
                                 AnalyzerSignal signal,
                                 float level_db,
                                 float hz);

/**
 * Choose which equaliser is active. Returns false for a null session.
 *
 * Both equalisers are kept across a switch, so moving to the parametric and
 * back does not lose the graphic's fader positions.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_set_eq_mode(struct AnalyzerSession *session, AnalyzerEqMode mode);

/**
 * Bands the active equaliser has. Zero when it is off.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
uintptr_t analyzer_session_eq_band_count(const struct AnalyzerSession *session);

/**
 * Read one band.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or writable.
 */
bool analyzer_session_eq_get_band(const struct AnalyzerSession *session,
                                  uintptr_t index,
                                  struct AnalyzerBand *out);

/**
 * Replace one band.
 *
 * # Safety
 *
 * `session` must be null or live; `band` must be null or readable.
 */
bool analyzer_session_eq_set_band(struct AnalyzerSession *session,
                                  uintptr_t index,
                                  const struct AnalyzerBand *band);

/**
 * Set one band's gain, which is all a graphic equaliser can change.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_eq_set_gain(struct AnalyzerSession *session, uintptr_t index, float gain_db);

/**
 * Append a band to the parametric equaliser. Returns its index, or -1.
 *
 * # Safety
 *
 * `session` must be null or live; `band` must be null or readable.
 */
intptr_t analyzer_session_eq_add_band(struct AnalyzerSession *session,
                                      const struct AnalyzerBand *band);

/**
 * Remove a band from the parametric equaliser.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_eq_remove_band(struct AnalyzerSession *session, uintptr_t index);

/**
 * Set every gain to zero and clear the trim.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_eq_flatten(struct AnalyzerSession *session);

/**
 * Trim the output so the equaliser's loudest point sits at unity.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_eq_trim(struct AnalyzerSession *session);

/**
 * Set the output trim by hand, in decibels.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_eq_set_preamp(struct AnalyzerSession *session, float db);

/**
 * Read the active equaliser's headroom.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or writable.
 */
bool analyzer_session_eq_info(const struct AnalyzerSession *session, struct AnalyzerEqInfo *out);

/**
 * Copy the equaliser's own curve, one value per pixel column, in decibels.
 *
 * Pass `band` of -1 for the combined curve, or a band index for that band
 * alone. Returns zero when no equaliser is active.
 *
 * This is arithmetic on the coefficients, not a measurement: it works whether
 * or not audio is running, which is what makes designing a correction against
 * a saved measurement possible.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or point to at least
 * `capacity` writable floats.
 */
uintptr_t analyzer_session_copy_eq_curve(struct AnalyzerSession *session,
                                         intptr_t band,
                                         float *out,
                                         uintptr_t capacity);

/**
 * Copy the measured trace with the equaliser applied.
 *
 * The point of an equaliser in a measurement tool: what the room would look
 * like after the correction, drawn beside what it looks like now. Adding
 * decibels is exact here because the equaliser's curve is known analytically
 * rather than measured.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or point to at least
 * `capacity` writable floats.
 */
uintptr_t analyzer_session_copy_corrected(struct AnalyzerSession *session,
                                          float *out,
                                          uintptr_t capacity);

/**
 * Map a phase in degrees to a pixel row.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
float analyzer_phase_to_y(const struct AnalyzerSession *session, float degrees);

/**
 * Map a coherence value to a pixel row.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
float analyzer_coherence_to_y(const struct AnalyzerSession *session, float value);

/**
 * Copy the long-term average trace, one value per pixel column.
 *
 * Uses the same geometry and reduction as `analyzer_session_copy_trace`, so
 * the two curves overlay exactly rather than being subtly offset from each
 * other.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or point to at least
 * `capacity` writable floats.
 */
uintptr_t analyzer_session_copy_average(struct AnalyzerSession *session,
                                        float *out,
                                        uintptr_t capacity);

/**
 * Restart the long-term average, leaving the live trace running.
 *
 * Takes effect on the analysis thread's next pass, so the very next frame may
 * still show the old average.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_reset_average(struct AnalyzerSession *session);

/**
 * Read metadata about the newest frame.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or writable.
 */
bool analyzer_session_frame_info(struct AnalyzerSession *session, struct AnalyzerFrameInfo *out);

/**
 * Measure harmonic distortion in the current live spectrum.
 *
 * Returns false when no fundamental stands far enough above the noise floor for
 * the figure to mean anything — distortion of a room's background hiss is not a
 * number worth showing, and a UI should hide the readout rather than display
 * a plausible-looking one.
 *
 * `fundamental_hz` may be zero to use the loudest peak, or set explicitly when
 * the stimulus frequency is known.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must be null or writable.
 */
bool analyzer_session_distortion(struct AnalyzerSession *session,
                                 float fundamental_hz,
                                 struct AnalyzerDistortion *out);

/**
 * Save the current spectrum to a measurement file.
 *
 * Writes a magnitude-only measurement, because that is what an RTA produces -
 * squaring the magnitude discarded the phase, and a file claiming zero phase
 * would be indistinguishable from one that measured it.
 *
 * Whether the levels are dB SPL or dBFS is recorded rather than assumed:
 * `spl_offset_db` is stored when non-zero and omitted otherwise, so a reader
 * can tell a calibrated measurement from an uncalibrated one.
 *
 * # Safety
 *
 * `session` must be null or live. `path` and `name` must be NUL-terminated
 * strings. `status` must be null or writable.
 */
bool analyzer_session_save_measurement(struct AnalyzerSession *session,
                                       const char *path,
                                       const char *name,
                                       float spl_offset_db,
                                       struct AnalyzerStatus *status);

/**
 * Export the current spectrum as REW-compatible text.
 *
 * # Safety
 *
 * As `analyzer_session_save_measurement`.
 */
bool analyzer_session_export_text(struct AnalyzerSession *session,
                                  const char *path,
                                  const char *name,
                                  float spl_offset_db,
                                  struct AnalyzerStatus *status);

/**
 * Copy the captured device's name into a caller buffer, returning its length.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must point to at least `capacity`
 * writable bytes.
 */
uintptr_t analyzer_session_device_name(const struct AnalyzerSession *session,
                                       char *out,
                                       uintptr_t capacity);

/**
 * Pixel position of a frequency. NaN if the session is null.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
float analyzer_freq_to_x(const struct AnalyzerSession *session, float hz);

/**
 * Frequency at a pixel position. NaN if the session is null.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
float analyzer_x_to_freq(const struct AnalyzerSession *session, float x);

/**
 * Pixel position of a level, zero being the top. NaN if the session is null.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
float analyzer_db_to_y(const struct AnalyzerSession *session, float db);

/**
 * Level at a pixel position. NaN if the session is null.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
float analyzer_y_to_db(const struct AnalyzerSession *session, float y);

/**
 * Copy frequency gridlines into `out`, returning how many were written.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must hold at least `capacity` ticks.
 */
uintptr_t analyzer_frequency_ticks(const struct AnalyzerSession *session,
                                   struct AnalyzerTick *out,
                                   uintptr_t capacity);

/**
 * Copy level gridlines every `step_db` into `out`.
 *
 * # Safety
 *
 * `session` must be null or live; `out` must hold at least `capacity` ticks.
 */
uintptr_t analyzer_level_ticks(const struct AnalyzerSession *session,
                               float step_db,
                               struct AnalyzerTick *out,
                               uintptr_t capacity);

/**
 * The target a fresh session starts with.
 */
struct AnalyzerTarget analyzer_target_default(void);

/**
 * Read the session's target.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must be writable.
 */
bool analyzer_session_target(const struct AnalyzerSession *session, struct AnalyzerTarget *out);

/**
 * Replace the session's target shape.
 *
 * Selecting `AnalyzerTargetShape::Custom` without a loaded curve leaves the
 * shape flat rather than silently evaluating to nothing.
 *
 * # Safety
 *
 * `session` must be null or live. `target` must be readable.
 */
bool analyzer_session_set_target(struct AnalyzerSession *session,
                                 const struct AnalyzerTarget *target);

/**
 * Load a custom target from a frequency/level text file and select it.
 *
 * # Safety
 *
 * `session` must be null or live. `path` must be a NUL-terminated C string.
 * `status` must be null or writable.
 */
bool analyzer_session_load_target(struct AnalyzerSession *session,
                                  const char *path,
                                  struct AnalyzerStatus *status);

/**
 * Align the target to the current measurement over the default band.
 *
 * A target is relative, so without this it floats somewhere unrelated to the
 * measurement and every error computed against it is dominated by a constant.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_align_target(struct AnalyzerSession *session);

/**
 * Copy the target curve, one level per pixel column.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must point to `capacity` writable
 * floats.
 */
uintptr_t analyzer_session_copy_target(struct AnalyzerSession *session,
                                       float *out,
                                       uintptr_t capacity);

/**
 * The measurement settings a UI should start from.
 */
struct AnalyzerMeasureConfig analyzer_measure_config_default(void);

/**
 * Start a swept measurement.
 *
 * Arms the recording before the sweep so nothing is missed, then sets the
 * generator. Fails when the session has no output stream to play through,
 * which is the common case on a laptop whose input and output are separate
 * devices.
 *
 * # Safety
 *
 * `session` must be null or live. `config` must be readable. `status` must be
 * null or writable.
 */
bool analyzer_session_start_measurement(struct AnalyzerSession *session,
                                        const struct AnalyzerMeasureConfig *config,
                                        struct AnalyzerStatus *status);

/**
 * How far a running measurement has got.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must be writable.
 */
bool analyzer_session_measure_progress(const struct AnalyzerSession *session,
                                       struct AnalyzerMeasureProgress *out);

/**
 * Abandon a measurement in progress and silence the generator.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
void analyzer_session_cancel_measurement(struct AnalyzerSession *session);

/**
 * Finish a measurement whose recording is complete.
 *
 * Returns false while the sweep is still playing, so polling this cannot
 * deconvolve half a sweep and report the result as a measurement.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must be null or writable. `status`
 * must be null or writable.
 */
bool analyzer_session_finish_measurement(struct AnalyzerSession *session,
                                         struct AnalyzerMeasureResult *out,
                                         struct AnalyzerStatus *status);

/**
 * Whether a completed measurement is held.
 *
 * # Safety
 *
 * `session` must be null or live.
 */
bool analyzer_session_has_measurement(const struct AnalyzerSession *session);

/**
 * Read the last measurement's findings.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must be writable.
 */
bool analyzer_session_measurement_result(const struct AnalyzerSession *session,
                                         struct AnalyzerMeasureResult *out);

/**
 * Copy the measured response, reduced onto the current axis.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must point to `capacity` writable
 * floats.
 */
uintptr_t analyzer_session_copy_measured(struct AnalyzerSession *session,
                                         float *out,
                                         uintptr_t capacity);

/**
 * Copy the impulse response itself, decimated to `capacity` points.
 *
 * Values are amplitudes, normalised to the peak, so a caller can draw the
 * impulse without knowing the recording level.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must point to `capacity` writable
 * floats.
 */
uintptr_t analyzer_session_copy_impulse(const struct AnalyzerSession *session,
                                        float *out,
                                        uintptr_t capacity,
                                        float seconds);

/**
 * Frequency gridlines for an axis of arbitrary length.
 *
 * The spectrogram runs frequency up the drawable rather than across it, so it
 * needs tick positions along a different length from the one the trace plot
 * declared. This builds a temporary axis over the session's current frequency
 * range and leaves the session's own geometry untouched, so asking does not
 * disturb what the trace plot is drawing.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must point to `capacity` writable
 * `AnalyzerTick` values.
 */
uintptr_t analyzer_frequency_ticks_for(const struct AnalyzerSession *session,
                                       float length,
                                       struct AnalyzerTick *out,
                                       uintptr_t capacity);

/**
 * Reduce the newest frame onto `rows` frequency positions.
 *
 * Values are decibels, not colours: mapping level to colour is the renderer's
 * job and differs per platform. Returns the number of rows written, which is
 * zero until something has been analysed.
 *
 * # Safety
 *
 * `session` must be null or live. `out` must point to `rows` writable floats.
 */
uintptr_t analyzer_session_copy_spectrogram_column(struct AnalyzerSession *session,
                                                   float *out,
                                                   uintptr_t rows);

/**
 * Create a trace store. Outlives any session; destroy it with
 * `analyzer_trace_store_destroy`.
 */
struct AnalyzerTraceStore *analyzer_trace_store_create(void);

/**
 * Release a trace store. Safe to call with null.
 *
 * # Safety
 *
 * `store` must come from `analyzer_trace_store_create` and not already be
 * destroyed.
 */
void analyzer_trace_store_destroy(struct AnalyzerTraceStore *store);

/**
 * Capture the session's live curve into the store.
 *
 * Returns its index, or -1 when there is nothing analysed yet. `name` may be
 * null, in which case a unique one is generated.
 *
 * # Safety
 *
 * `store` and `session` must be null or live. `name` must be null or a
 * NUL-terminated C string.
 */
intptr_t analyzer_trace_store_capture(struct AnalyzerTraceStore *store,
                                      struct AnalyzerSession *session,
                                      const char *name);

/**
 * How many traces are held.
 *
 * # Safety
 *
 * `store` must be null or live.
 */
uintptr_t analyzer_trace_store_count(const struct AnalyzerTraceStore *store);

/**
 * Describe one trace.
 *
 * # Safety
 *
 * `store` must be null or live. `out` must be writable.
 */
bool analyzer_trace_store_info(const struct AnalyzerTraceStore *store,
                               uintptr_t index,
                               struct AnalyzerTraceInfo *out);

/**
 * Show or hide a trace.
 *
 * # Safety
 *
 * `store` must be null or live.
 */
bool analyzer_trace_store_set_visible(struct AnalyzerTraceStore *store,
                                      uintptr_t index,
                                      bool visible);

/**
 * Forget a trace.
 *
 * # Safety
 *
 * `store` must be null or live.
 */
bool analyzer_trace_store_remove(struct AnalyzerTraceStore *store, uintptr_t index);

/**
 * Forget every trace.
 *
 * # Safety
 *
 * `store` must be null or live.
 */
void analyzer_trace_store_clear(struct AnalyzerTraceStore *store);

/**
 * Copy a captured trace, reduced onto the session's current axis.
 *
 * The session supplies only the geometry. A trace captured at one transform
 * size draws correctly against a session running at another, which is the
 * point of storing it at analysis resolution.
 *
 * # Safety
 *
 * `store` and `session` must be null or live. `out` must point to `capacity`
 * writable floats.
 */
uintptr_t analyzer_trace_store_copy(struct AnalyzerTraceStore *store,
                                    uintptr_t index,
                                    const struct AnalyzerSession *session,
                                    float *out,
                                    uintptr_t capacity);

/**
 * The fit constraints a fresh session starts with.
 */
struct AnalyzerOptimiserConfig analyzer_optimiser_config_default(void);

/**
 * Fit filters to the gap between the measurement and the target.
 *
 * The result **replaces** the parametric equaliser's bands and selects it, so
 * the fit is immediately drawn and heard. Replacing rather than appending is
 * deliberate: running the fit twice should give the same answer as running it
 * once, and appending would instead correct the correction.
 *
 * # Safety
 *
 * `session` must be null or live. `config` must be readable. `out` must be
 * null or writable. `status` must be null or writable.
 */
bool analyzer_session_optimise(struct AnalyzerSession *session,
                               const struct AnalyzerOptimiserConfig *config,
                               struct AnalyzerOptimisation *out,
                               struct AnalyzerStatus *status);

/**
 * Write the active equaliser to `path` in `format`.
 *
 * Fails when no equaliser is active, rather than writing an empty file that
 * looks like a successful export of nothing.
 *
 * # Safety
 *
 * `session` must be null or live. `path` must be a NUL-terminated C string.
 * `status` must be null or writable.
 */
bool analyzer_session_export_filters(const struct AnalyzerSession *session,
                                     AnalyzerFilterFormat format,
                                     const char *path,
                                     struct AnalyzerStatus *status);

/**
 * Write a generated signal to `path`.
 *
 * `level_db` is in dBFS and capped at 0: a generator that can write above full
 * scale can only write something that clips. `hz` is the tone frequency or the
 * sweep start; `end_hz` is used only by a sweep.
 *
 * Returns the number of frames written, or 0 on failure with `status` set.
 *
 * # Safety
 *
 * `path` must be a NUL-terminated C string. `status` must be null or writable.
 */
uintptr_t analyzer_write_signal(const char *path,
                                AnalyzerSignal signal,
                                float hz,
                                float end_hz,
                                float level_db,
                                float seconds,
                                float sample_rate,
                                AnalyzerSampleDepth depth,
                                struct AnalyzerStatus *status);

/**
 * The settings a fresh install starts with.
 */
struct AnalyzerSettings analyzer_settings_default(void);

/**
 * Read settings from `path`.
 *
 * A file that does not exist is not a failure: it is the first launch, and the
 * defaults are written through with a success status. Anything else - an
 * unreadable directory, a permissions problem - is reported, because silently
 * starting from defaults there would look identical to the settings having been
 * lost.
 *
 * # Safety
 *
 * `path` must be a NUL-terminated C string. `out` must point to a writable
 * `AnalyzerSettings`. `status` must be null or writable.
 */
bool analyzer_settings_load(const char *path,
                            struct AnalyzerSettings *out,
                            struct AnalyzerStatus *status);

/**
 * Write settings to `path`, creating the containing directory if needed.
 *
 * # Safety
 *
 * `path` must be a NUL-terminated C string. `settings` must point to a readable
 * `AnalyzerSettings`. `status` must be null or writable.
 */
bool analyzer_settings_save(const char *path,
                            const struct AnalyzerSettings *settings,
                            struct AnalyzerStatus *status);

#endif  /* ANALYZER_H */
