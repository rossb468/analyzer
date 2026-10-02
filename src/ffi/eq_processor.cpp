#include "ffi/eq_processor.hpp"

#include <cmath>

namespace analyzer::ffi {

EqCoefficients EqCoefficients::from_equaliser(std::uint64_t generation, const dsp::Equaliser& eq) {
    EqCoefficients out;
    out.generation = generation;
    out.trim = db_to_amplitude(eq.preamp_db());
    for (const dsp::FilterBand& band : eq.bands()) {
        if (out.count == kMaxEqBands) {
            break;
        }
        out.sections[out.count] = band.design(eq.sample_rate());
        ++out.count;
    }
    return out;
}

void EqProcessor::process(std::span<float> samples) noexcept {
    // A reference into the triple buffer, valid until the next read, which is
    // the next call. Nothing here outlives that.
    const EqCoefficients& latest = reader_.read();
    if (latest.generation != generation_) {
        generation_ = latest.generation;
        count_ = latest.count;
        trim_ = latest.trim;
        for (std::size_t i = 0; i < sections_.size(); ++i) {
            // Coefficients only. Replacing the whole section would reset the
            // delay line, and a filter restarted mid-signal clicks.
            sections_[i].b0 = latest.sections[i].b0;
            sections_[i].b1 = latest.sections[i].b1;
            sections_[i].b2 = latest.sections[i].b2;
            sections_[i].a1 = latest.sections[i].a1;
            sections_[i].a2 = latest.sections[i].a2;
        }
    }

    for (std::size_t i = 0; i < count_; ++i) {
        sections_[i].process_block(samples);
    }
    if (trim_ != 1.0f) {
        for (float& sample : samples) {
            sample *= trim_;
        }
    }
}

}  // namespace analyzer::ffi
