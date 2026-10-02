#include "ffi/convert.hpp"

#include "ffi/internal.hpp"

namespace analyzer::ffi {

dsp::WindowKind to_window_kind(AnalyzerWindow window) noexcept {
    switch (window) {
        case AnalyzerWindow_Rectangular: return dsp::WindowKind::rectangular();
        case AnalyzerWindow_Hann: return dsp::WindowKind::hann();
        case AnalyzerWindow_BlackmanHarris: return dsp::WindowKind::blackman_harris();
        case AnalyzerWindow_FlatTop: return dsp::WindowKind::flat_top();
        case AnalyzerWindow_Tukey: return dsp::WindowKind::tukey(0.25f);
    }
    return dsp::WindowKind::hann();
}

dsp::Overlap to_overlap(AnalyzerOverlap overlap) noexcept {
    switch (overlap) {
        case AnalyzerOverlap_None: return dsp::Overlap::None;
        case AnalyzerOverlap_Half: return dsp::Overlap::Half;
        case AnalyzerOverlap_ThreeQuarters: return dsp::Overlap::ThreeQuarters;
        case AnalyzerOverlap_SevenEighths: return dsp::Overlap::SevenEighths;
    }
    return dsp::Overlap::ThreeQuarters;
}

plot::Reduction to_reduction(AnalyzerReduction reduction) noexcept {
    switch (reduction) {
        case AnalyzerReduction_Max: return plot::Reduction::Max;
        case AnalyzerReduction_Mean: return plot::Reduction::Mean;
    }
    return plot::Reduction::Max;
}

dsp::FilterKind to_filter_kind(AnalyzerFilterKind kind) noexcept {
    switch (kind) {
        case AnalyzerFilterKind_Peaking: return dsp::FilterKind::Peaking;
        case AnalyzerFilterKind_LowShelf: return dsp::FilterKind::LowShelf;
        case AnalyzerFilterKind_HighShelf: return dsp::FilterKind::HighShelf;
        case AnalyzerFilterKind_LowPass: return dsp::FilterKind::LowPass;
        case AnalyzerFilterKind_HighPass: return dsp::FilterKind::HighPass;
        case AnalyzerFilterKind_BandPass: return dsp::FilterKind::BandPass;
        case AnalyzerFilterKind_Notch: return dsp::FilterKind::Notch;
        case AnalyzerFilterKind_AllPass: return dsp::FilterKind::AllPass;
    }
    return dsp::FilterKind::Peaking;
}

AnalyzerFilterKind to_c(dsp::FilterKind kind) noexcept {
    switch (kind) {
        case dsp::FilterKind::Peaking: return AnalyzerFilterKind_Peaking;
        case dsp::FilterKind::LowShelf: return AnalyzerFilterKind_LowShelf;
        case dsp::FilterKind::HighShelf: return AnalyzerFilterKind_HighShelf;
        case dsp::FilterKind::LowPass: return AnalyzerFilterKind_LowPass;
        case dsp::FilterKind::HighPass: return AnalyzerFilterKind_HighPass;
        case dsp::FilterKind::BandPass: return AnalyzerFilterKind_BandPass;
        case dsp::FilterKind::Notch: return AnalyzerFilterKind_Notch;
        case dsp::FilterKind::AllPass: return AnalyzerFilterKind_AllPass;
    }
    return AnalyzerFilterKind_Peaking;
}

dsp::FilterBand to_filter_band(const AnalyzerBand& band) noexcept {
    dsp::FilterBand out;
    out.kind = to_filter_kind(band.kind);
    out.hz = band.hz;
    out.gain_db = band.gain_db;
    out.q = band.q;
    out.enabled = band.enabled;
    return out;
}

AnalyzerBand to_c(const dsp::FilterBand& band) noexcept {
    AnalyzerBand out{};
    out.kind = to_c(band.kind);
    out.hz = band.hz;
    out.gain_db = band.gain_db;
    out.q = band.q;
    out.enabled = band.enabled;
    return out;
}

model::FilterFormat to_filter_format(AnalyzerFilterFormat format) noexcept {
    switch (format) {
        case AnalyzerFilterFormat_Rew: return model::FilterFormat::Rew;
        case AnalyzerFilterFormat_EqualizerApo: return model::FilterFormat::EqualizerApo;
        case AnalyzerFilterFormat_MiniDsp: return model::FilterFormat::MiniDsp;
    }
    return model::FilterFormat::Rew;
}

model::SampleDepth to_sample_depth(AnalyzerSampleDepth depth) noexcept {
    switch (depth) {
        case AnalyzerSampleDepth_Int16: return model::SampleDepth::Int16;
        case AnalyzerSampleDepth_Int24: return model::SampleDepth::Int24;
        case AnalyzerSampleDepth_Float32: return model::SampleDepth::Float32;
    }
    return model::SampleDepth::Float32;
}

AnalyzerSettings to_c(const model::Settings& settings) noexcept {
    AnalyzerSettings out{};
    out.fft_size = settings.fft_size;
    switch (settings.window) {
        case model::WindowChoice::Rectangular: out.window = AnalyzerWindow_Rectangular; break;
        case model::WindowChoice::Hann: out.window = AnalyzerWindow_Hann; break;
        case model::WindowChoice::BlackmanHarris: out.window = AnalyzerWindow_BlackmanHarris; break;
        case model::WindowChoice::FlatTop: out.window = AnalyzerWindow_FlatTop; break;
    }
    switch (settings.averaging) {
        case model::AveragingChoice::None: out.averaging = AnalyzerAveraging_None; break;
        case model::AveragingChoice::Fast: out.averaging = AnalyzerAveraging_Fast; break;
        case model::AveragingChoice::Infinite: out.averaging = AnalyzerAveraging_Infinite; break;
        case model::AveragingChoice::PeakHold: out.averaging = AnalyzerAveraging_PeakHold; break;
    }
    out.start_on_launch = settings.start_on_launch;
    out.min_hz = settings.min_hz;
    out.max_hz = settings.max_hz;
    out.min_db = settings.min_db;
    out.max_db = settings.max_db;
    out.level_grid_step = settings.level_grid_step;
    out.has_spl_offset = settings.spl_offset_db.has_value();
    out.spl_offset_db = settings.spl_offset_db.value_or(0.0f);
    write_c_string(out.mic_cal_path, settings.mic_cal_path.value_or(""));
    return out;
}

model::Settings to_settings(const AnalyzerSettings& settings) {
    const std::string path = read_c_string(settings.mic_cal_path);

    model::Settings out;
    out.fft_size = settings.fft_size;
    switch (settings.window) {
        case AnalyzerWindow_Rectangular: out.window = model::WindowChoice::Rectangular; break;
        case AnalyzerWindow_BlackmanHarris: out.window = model::WindowChoice::BlackmanHarris; break;
        case AnalyzerWindow_FlatTop: out.window = model::WindowChoice::FlatTop; break;
        // A Tukey window is a shape the DSP has and the preferences vocabulary
        // does not; it degrades to the default rather than being stored as
        // something that cannot be read back.
        case AnalyzerWindow_Hann:
        case AnalyzerWindow_Tukey: out.window = model::WindowChoice::Hann; break;
    }
    switch (settings.averaging) {
        case AnalyzerAveraging_None: out.averaging = model::AveragingChoice::None; break;
        case AnalyzerAveraging_Fast: out.averaging = model::AveragingChoice::Fast; break;
        case AnalyzerAveraging_Infinite: out.averaging = model::AveragingChoice::Infinite; break;
        case AnalyzerAveraging_PeakHold: out.averaging = model::AveragingChoice::PeakHold; break;
    }
    out.start_on_launch = settings.start_on_launch;
    out.min_hz = settings.min_hz;
    out.max_hz = settings.max_hz;
    out.min_db = settings.min_db;
    out.max_db = settings.max_db;
    out.level_grid_step = settings.level_grid_step;
    if (settings.has_spl_offset) {
        out.spl_offset_db = settings.spl_offset_db;
    }
    if (!path.empty()) {
        out.mic_cal_path = path;
    }
    return out.validated();
}

}  // namespace analyzer::ffi
