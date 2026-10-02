#include "dsp/eq.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

// A magnitude as decibels, floored far below anything audible so that a band
// that nulls a frequency exactly reads as a very deep cut, not minus infinity.
float level_db(float magnitude) noexcept {
    return amplitude_to_db(std::max(magnitude, 1e-12f));
}

// Replace a section's coefficients and leave its delay line alone, so that
// changing a band does not click.
void set_coefficients(Biquad& section, const Biquad& designed) noexcept {
    section.b0 = designed.b0;
    section.b1 = designed.b1;
    section.b2 = designed.b2;
    section.a1 = designed.a1;
    section.a2 = designed.a2;
}

}  // namespace

bool uses_gain(FilterKind kind) noexcept {
    switch (kind) {
        case FilterKind::Peaking:
        case FilterKind::LowShelf:
        case FilterKind::HighShelf: return true;
        case FilterKind::LowPass:
        case FilterKind::HighPass:
        case FilterKind::BandPass:
        case FilterKind::Notch:
        case FilterKind::AllPass: return false;
    }
    return false;
}

const char* label(FilterKind kind) noexcept {
    switch (kind) {
        case FilterKind::Peaking: return "PK";
        case FilterKind::LowShelf: return "LS";
        case FilterKind::HighShelf: return "HS";
        case FilterKind::LowPass: return "LP";
        case FilterKind::HighPass: return "HP";
        case FilterKind::BandPass: return "BP";
        case FilterKind::Notch: return "NO";
        case FilterKind::AllPass: return "AP";
    }
    return "??";
}

const char* to_string(FilterKind kind) noexcept {
    switch (kind) {
        case FilterKind::Peaking: return "Peaking";
        case FilterKind::LowShelf: return "LowShelf";
        case FilterKind::HighShelf: return "HighShelf";
        case FilterKind::LowPass: return "LowPass";
        case FilterKind::HighPass: return "HighPass";
        case FilterKind::BandPass: return "BandPass";
        case FilterKind::Notch: return "Notch";
        case FilterKind::AllPass: return "AllPass";
    }
    return "unknown";
}

bool FilterBand::is_transparent() const noexcept {
    return !enabled || (uses_gain(kind) && gain_db == 0.0f);
}

Biquad FilterBand::design(float sample_rate) const noexcept {
    if (is_transparent()) {
        return Biquad::identity();
    }
    switch (kind) {
        case FilterKind::Peaking: return Biquad::peaking(hz, q, gain_db, sample_rate);
        case FilterKind::LowShelf: return Biquad::low_shelf(hz, q, gain_db, sample_rate);
        case FilterKind::HighShelf: return Biquad::high_shelf(hz, q, gain_db, sample_rate);
        case FilterKind::LowPass: return Biquad::low_pass(hz, q, sample_rate);
        case FilterKind::HighPass: return Biquad::high_pass(hz, q, sample_rate);
        case FilterKind::BandPass: return Biquad::band_pass(hz, q, sample_rate);
        case FilterKind::Notch: return Biquad::notch(hz, q, sample_rate);
        case FilterKind::AllPass: return Biquad::all_pass(hz, q, sample_rate);
    }
    return Biquad::identity();
}

Equaliser::Equaliser(float sample_rate, std::vector<FilterBand> bands)
    : bands_(std::move(bands)), sample_rate_(sample_rate) {
    redesign();
}

Equaliser Equaliser::graphic(float sample_rate) {
    std::vector<FilterBand> bands;
    bands.reserve(kOctaveCentres.size());
    for (const float hz : kOctaveCentres) {
        bands.push_back(FilterBand::peaking(hz, 0.0f, kOctaveQ));
    }
    return Equaliser(sample_rate, std::move(bands));
}

Equaliser Equaliser::parametric(float sample_rate) {
    return Equaliser(sample_rate, {});
}

void Equaliser::set_preamp_db(float db) noexcept {
    preamp_db_ = std::isfinite(db) ? db : 0.0f;
}

void Equaliser::set_band(std::size_t index, const FilterBand& band) noexcept {
    if (index >= bands_.size() || bands_[index] == band) {
        return;
    }
    bands_[index] = band;
    // Only this section changes, and only its coefficients: the state is
    // left alone so a fader move does not click.
    if (index < sections_.size()) {
        set_coefficients(sections_[index], band.design(sample_rate_));
    }
}

void Equaliser::set_gain_db(std::size_t index, float gain_db) noexcept {
    if (index >= bands_.size()) {
        return;
    }
    FilterBand band = bands_[index];
    band.gain_db = std::isfinite(gain_db) ? gain_db : 0.0f;
    set_band(index, band);
}

std::size_t Equaliser::push_band(const FilterBand& band) {
    bands_.push_back(band);
    sections_.push_back(band.design(sample_rate_));
    return bands_.size() - 1;
}

void Equaliser::remove_band(std::size_t index) {
    if (index >= bands_.size()) {
        return;
    }
    bands_.erase(bands_.begin() + static_cast<std::ptrdiff_t>(index));
    sections_.erase(sections_.begin() + static_cast<std::ptrdiff_t>(index));
}

void Equaliser::set_sample_rate(float sample_rate) {
    if (sample_rate <= 0.0f || sample_rate == sample_rate_) {
        return;
    }
    sample_rate_ = sample_rate;
    redesign();
}

void Equaliser::set_bands(std::vector<FilterBand> bands) {
    bands_ = std::move(bands);
    sections_.assign(bands_.size(), Biquad::identity());
    redesign();
}

Complex32 Equaliser::response_at(float hz) const noexcept {
    Complex32 response(db_to_amplitude(preamp_db_), 0.0f);
    for (const Biquad& section : sections_) {
        response *= section.response_at(hz, sample_rate_);
    }
    return response;
}

float Equaliser::magnitude_db_at(float hz) const noexcept {
    return level_db(std::abs(response_at(hz)));
}

float Equaliser::band_magnitude_db_at(std::size_t index, float hz) const noexcept {
    if (index >= sections_.size()) {
        return 0.0f;
    }
    return level_db(sections_[index].magnitude_at(hz, sample_rate_));
}

void Equaliser::write_magnitude_db(std::span<const float> frequencies,
                                   std::span<float> out) const noexcept {
    const std::size_t count = std::min(frequencies.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = magnitude_db_at(frequencies[i]);
    }
}

void Equaliser::write_phase_degrees(std::span<const float> frequencies,
                                    std::span<float> out) const noexcept {
    const std::size_t count = std::min(frequencies.size(), out.size());
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = std::arg(response_at(frequencies[i])) * 180.0f / std::numbers::pi_v<float>;
    }
}

float Equaliser::peak_gain_db() const noexcept {
    constexpr std::size_t kPoints = 512;
    constexpr float low = 10.0f;
    const float high = std::min(sample_rate_ * 0.5f, 24'000.0f);
    if (high <= low) {
        return preamp_db_;
    }
    const float ratio = std::log(high / low);
    float peak = -std::numeric_limits<float>::infinity();
    for (std::size_t i = 0; i < kPoints; ++i) {
        const float hz =
            low * std::exp(ratio * static_cast<float>(i) / static_cast<float>(kPoints - 1));
        peak = std::max(peak, magnitude_db_at(hz));
    }
    return peak;
}

void Equaliser::trim_to_unity() noexcept {
    const float peak = peak_gain_db();
    // A hundredth of a decibel, not zero. A cut-only equaliser evaluates to
    // a peak a millionth of a decibel above unity through ordinary rounding,
    // and trimming by that would leave a preamp value that looks like a bug.
    if (std::isfinite(peak) && peak > 0.01f) {
        preamp_db_ -= peak;
    }
}

void Equaliser::process(std::span<float> samples) noexcept {
    for (Biquad& section : sections_) {
        section.process_block(samples);
    }
    const float trim = db_to_amplitude(preamp_db_);
    if (trim != 1.0f) {
        for (float& sample : samples) {
            sample *= trim;
        }
    }
}

void Equaliser::reset() noexcept {
    for (Biquad& section : sections_) {
        section.reset();
    }
}

void Equaliser::flatten() noexcept {
    for (std::size_t index = 0; index < bands_.size(); ++index) {
        set_gain_db(index, 0.0f);
    }
    preamp_db_ = 0.0f;
}

void Equaliser::redesign() {
    sections_.resize(bands_.size(), Biquad::identity());
    for (std::size_t i = 0; i < bands_.size(); ++i) {
        set_coefficients(sections_[i], bands_[i].design(sample_rate_));
    }
}

}  // namespace analyzer::dsp
