/*
 * Compiles include/analyzer.h as C and links the whole C ABI.
 *
 * The apps import this header from Swift, which is a C importer, so the header
 * has to stay plain C: no C++ guards, no C++ keywords, and an enum declared
 * exactly once. This file includes it from a C translation unit under -std=c11
 * and -std=c23 (CMake builds it both ways), with warnings as errors, so a
 * change that is fine for the C++ implementation but wrong for C fails here.
 *
 * It also takes the address of every entry point, which makes the linker prove
 * that the library defines all of them, and checks the ABI facts that must not
 * drift: sizes, offsets, enumerator values and the defaults a UI starts from.
 */

#include "analyzer.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Sizes and offsets the Swift side relies on. */
_Static_assert(ANALYZER_MESSAGE_LEN == 256, "status message length");
_Static_assert(ANALYZER_MAX_EQ_BANDS == 24, "equaliser band limit");
_Static_assert(ANALYZER_MAX_HARMONICS == 10, "harmonic count");
_Static_assert(ANALYZER_TRACE_NAME_LEN == 128, "trace name length");
_Static_assert(ANALYZER_PATH_LEN == 1024, "settings path length");
_Static_assert(sizeof(AnalyzerStatus) == 4 + ANALYZER_MESSAGE_LEN, "status size");
_Static_assert(offsetof(AnalyzerStatus, message) == 4, "status message offset");
_Static_assert(sizeof(AnalyzerTick) == 12, "tick: float, float, bool, padding");
_Static_assert(sizeof(AnalyzerWindow) == 4, "enums are uint32_t");
_Static_assert(sizeof(AnalyzerSignal) == 4, "enums are uint32_t");
_Static_assert(sizeof(AnalyzerEqMode) == 4, "enums are uint32_t");

/* Whole-struct layouts on a 64-bit target, where `uintptr_t` is eight bytes. The same
 * numbers are asserted from C++ in abi_test.cpp, which is what proves the
 * implementation's view of each struct is the one a C caller has. */
#if UINTPTR_MAX == 0xFFFFFFFFFFFFFFFFu
_Static_assert(sizeof(AnalyzerDevice) == 40 && _Alignof(AnalyzerDevice) == 8, "AnalyzerDevice layout");
_Static_assert(sizeof(AnalyzerSessionConfig) == 64 && _Alignof(AnalyzerSessionConfig) == 8, "AnalyzerSessionConfig layout");
_Static_assert(sizeof(AnalyzerStatus) == 260 && _Alignof(AnalyzerStatus) == 4, "AnalyzerStatus layout");
_Static_assert(sizeof(AnalyzerTransferInfo) == 20 && _Alignof(AnalyzerTransferInfo) == 4, "AnalyzerTransferInfo layout");
_Static_assert(sizeof(AnalyzerBand) == 20 && _Alignof(AnalyzerBand) == 4, "AnalyzerBand layout");
_Static_assert(sizeof(AnalyzerEqInfo) == 24 && _Alignof(AnalyzerEqInfo) == 8, "AnalyzerEqInfo layout");
_Static_assert(sizeof(AnalyzerFrameInfo) == 32 && _Alignof(AnalyzerFrameInfo) == 8, "AnalyzerFrameInfo layout");
_Static_assert(sizeof(AnalyzerDistortion) == 152 && _Alignof(AnalyzerDistortion) == 4, "AnalyzerDistortion layout");
_Static_assert(sizeof(AnalyzerTick) == 12 && _Alignof(AnalyzerTick) == 4, "AnalyzerTick layout");
_Static_assert(sizeof(AnalyzerTarget) == 24 && _Alignof(AnalyzerTarget) == 4, "AnalyzerTarget layout");
_Static_assert(sizeof(AnalyzerMeasureConfig) == 28 && _Alignof(AnalyzerMeasureConfig) == 4, "AnalyzerMeasureConfig layout");
_Static_assert(sizeof(AnalyzerMeasureProgress) == 32 && _Alignof(AnalyzerMeasureProgress) == 8, "AnalyzerMeasureProgress layout");
_Static_assert(sizeof(AnalyzerMeasureResult) == 56 && _Alignof(AnalyzerMeasureResult) == 8, "AnalyzerMeasureResult layout");
_Static_assert(sizeof(AnalyzerTraceInfo) == 152 && _Alignof(AnalyzerTraceInfo) == 8, "AnalyzerTraceInfo layout");
_Static_assert(sizeof(AnalyzerOptimiserConfig) == 32 && _Alignof(AnalyzerOptimiserConfig) == 4, "AnalyzerOptimiserConfig layout");
_Static_assert(sizeof(AnalyzerOptimisation) == 12 && _Alignof(AnalyzerOptimisation) == 4, "AnalyzerOptimisation layout");
_Static_assert(sizeof(AnalyzerSettings) == 1068 && _Alignof(AnalyzerSettings) == 4, "AnalyzerSettings layout");
#endif

/* Enumerator values are the ABI. */
_Static_assert(AnalyzerWindow_Hann == 1 && AnalyzerWindow_Tukey == 4, "window values");
_Static_assert(AnalyzerOverlap_SevenEighths == 3, "overlap values");
_Static_assert(AnalyzerAveraging_PeakHold == 3, "averaging values");
_Static_assert(AnalyzerMode_Transfer == 1, "mode values");
_Static_assert(AnalyzerReference_Input == 1, "reference values");
_Static_assert(AnalyzerSignal_Sweep == 4, "signal values");
_Static_assert(AnalyzerReduction_Mean == 1, "reduction values");
_Static_assert(AnalyzerCurve_Coherence == 2, "curve values");
_Static_assert(AnalyzerEqMode_Parametric == 2, "eq mode values");
_Static_assert(AnalyzerFilterKind_AllPass == 7, "filter kind values");
_Static_assert(AnalyzerTargetShape_Custom == 3, "target shape values");
_Static_assert(AnalyzerFilterFormat_MiniDsp == 2, "filter format values");
_Static_assert(AnalyzerSampleDepth_Float32 == 2, "sample depth values");

/* Every entry point, so that a missing definition is a link error. */
/* Not static: its only job is to be linked, and a compiler is entitled to drop an
 * unused static. */
void (*const entry_points[])(void) = {
    (void (*)(void))(analyzer_device_list_create),
    (void (*)(void))(analyzer_device_list_destroy),
    (void (*)(void))(analyzer_device_list_count),
    (void (*)(void))(analyzer_device_list_get),
    (void (*)(void))(analyzer_session_config_default),
    (void (*)(void))(analyzer_session_start),
    (void (*)(void))(analyzer_session_stop),
    (void (*)(void))(analyzer_session_set_plot),
    (void (*)(void))(analyzer_session_has_new_frame),
    (void (*)(void))(analyzer_session_copy_trace),
    (void (*)(void))(analyzer_session_copy_transfer),
    (void (*)(void))(analyzer_session_transfer_info),
    (void (*)(void))(analyzer_session_estimate_delay),
    (void (*)(void))(analyzer_session_set_delay),
    (void (*)(void))(analyzer_session_set_signal),
    (void (*)(void))(analyzer_session_set_eq_mode),
    (void (*)(void))(analyzer_session_eq_band_count),
    (void (*)(void))(analyzer_session_eq_get_band),
    (void (*)(void))(analyzer_session_eq_set_band),
    (void (*)(void))(analyzer_session_eq_set_gain),
    (void (*)(void))(analyzer_session_eq_add_band),
    (void (*)(void))(analyzer_session_eq_remove_band),
    (void (*)(void))(analyzer_session_eq_flatten),
    (void (*)(void))(analyzer_session_eq_trim),
    (void (*)(void))(analyzer_session_eq_set_preamp),
    (void (*)(void))(analyzer_session_eq_info),
    (void (*)(void))(analyzer_session_copy_eq_curve),
    (void (*)(void))(analyzer_session_copy_corrected),
    (void (*)(void))(analyzer_phase_to_y),
    (void (*)(void))(analyzer_coherence_to_y),
    (void (*)(void))(analyzer_session_copy_average),
    (void (*)(void))(analyzer_session_reset_average),
    (void (*)(void))(analyzer_session_frame_info),
    (void (*)(void))(analyzer_session_distortion),
    (void (*)(void))(analyzer_session_save_measurement),
    (void (*)(void))(analyzer_session_export_text),
    (void (*)(void))(analyzer_session_device_name),
    (void (*)(void))(analyzer_freq_to_x),
    (void (*)(void))(analyzer_x_to_freq),
    (void (*)(void))(analyzer_db_to_y),
    (void (*)(void))(analyzer_y_to_db),
    (void (*)(void))(analyzer_frequency_ticks),
    (void (*)(void))(analyzer_level_ticks),
    (void (*)(void))(analyzer_target_default),
    (void (*)(void))(analyzer_session_target),
    (void (*)(void))(analyzer_session_set_target),
    (void (*)(void))(analyzer_session_load_target),
    (void (*)(void))(analyzer_session_align_target),
    (void (*)(void))(analyzer_session_copy_target),
    (void (*)(void))(analyzer_measure_config_default),
    (void (*)(void))(analyzer_session_start_measurement),
    (void (*)(void))(analyzer_session_measure_progress),
    (void (*)(void))(analyzer_session_cancel_measurement),
    (void (*)(void))(analyzer_session_finish_measurement),
    (void (*)(void))(analyzer_session_has_measurement),
    (void (*)(void))(analyzer_session_measurement_result),
    (void (*)(void))(analyzer_session_copy_measured),
    (void (*)(void))(analyzer_session_copy_impulse),
    (void (*)(void))(analyzer_frequency_ticks_for),
    (void (*)(void))(analyzer_session_copy_spectrogram_column),
    (void (*)(void))(analyzer_trace_store_create),
    (void (*)(void))(analyzer_trace_store_destroy),
    (void (*)(void))(analyzer_trace_store_capture),
    (void (*)(void))(analyzer_trace_store_count),
    (void (*)(void))(analyzer_trace_store_info),
    (void (*)(void))(analyzer_trace_store_set_visible),
    (void (*)(void))(analyzer_trace_store_remove),
    (void (*)(void))(analyzer_trace_store_clear),
    (void (*)(void))(analyzer_trace_store_copy),
    (void (*)(void))(analyzer_optimiser_config_default),
    (void (*)(void))(analyzer_session_optimise),
    (void (*)(void))(analyzer_session_export_filters),
    (void (*)(void))(analyzer_write_signal),
    (void (*)(void))(analyzer_settings_default),
    (void (*)(void))(analyzer_settings_load),
    (void (*)(void))(analyzer_settings_save),
};

static int failures;

#define CHECK(condition)                                                    \
    do {                                                                    \
        if (!(condition)) {                                                 \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
                    #condition);                                            \
            ++failures;                                                     \
        }                                                                   \
    } while (0)

int main(void) {
    CHECK(sizeof(entry_points) / sizeof(entry_points[0]) == 76);

    /* The defaults a UI starts from. */
    AnalyzerSessionConfig config = analyzer_session_config_default();
    CHECK(config.device_uid == NULL);
    CHECK(config.fft_size == 4096);
    CHECK(config.mode == AnalyzerMode_Spectrum);
    CHECK(config.signal == AnalyzerSignal_Silence);
    CHECK(config.signal_level_db <= -12.0f);

    AnalyzerSettings settings = analyzer_settings_default();
    CHECK(settings.fft_size == 4096);
    CHECK(!settings.has_spl_offset);
    CHECK(settings.mic_cal_path[0] == '\0');

    AnalyzerTarget target = analyzer_target_default();
    CHECK(target.shape == AnalyzerTargetShape_Flat);

    AnalyzerMeasureConfig measure = analyzer_measure_config_default();
    CHECK(measure.start_hz < measure.end_hz);

    AnalyzerOptimiserConfig fit = analyzer_optimiser_config_default();
    CHECK(fit.max_boost_db < fit.max_cut_db);

    /* A null handle is an answer, not a crash. */
    AnalyzerStatus status;
    memset(&status, 0, sizeof status);
    CHECK(analyzer_session_start(NULL, &status) == NULL);
    CHECK(status.code != 0);
    CHECK(strcmp(status.message, "null configuration") == 0);

    analyzer_session_stop(NULL);
    analyzer_device_list_destroy(NULL);
    analyzer_trace_store_destroy(NULL);
    CHECK(analyzer_device_list_count(NULL) == 0);
    CHECK(!analyzer_session_has_new_frame(NULL));
    CHECK(analyzer_session_copy_trace(NULL, NULL, 0) == 0);
    CHECK(analyzer_session_eq_add_band(NULL, NULL) == -1);
    CHECK(analyzer_trace_store_capture(NULL, NULL, NULL) == -1);

    /* A store works without a session, through C. */
    AnalyzerTraceStore *store = analyzer_trace_store_create();
    CHECK(store != NULL);
    CHECK(analyzer_trace_store_count(store) == 0);
    analyzer_trace_store_destroy(store);

    return failures == 0 ? 0 : 1;
}
