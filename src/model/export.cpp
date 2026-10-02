#include "model/export.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <string_view>
#include <variant>

#include "base/number_text.hpp"

namespace analyzer::model {

namespace {

void line(std::string& out, std::string_view text) {
    out += text;
    out += '\n';
}

// Level of one complex bin, with the offset applied. A zero bin floors at
// -200 and the offset is not added to the floor: there is no level to correct.
double level_db(const Complex64& bin, double offset) {
    const double magnitude = std::abs(bin);
    return magnitude > 0.0 ? 20.0 * std::log10(magnitude) + offset : -200.0;
}

double phase_degrees(const Complex64& bin) {
    return text::to_degrees(std::arg(bin));
}

struct Writer {
    std::string& out;
    const Measurement& measurement;
    double offset;

    void operator()(const SpectrumData& spectrum) const {
        line(out, "* Freq(Hz) SPL(dB) Phase(degrees)");
        for (std::size_t index = 0; index < spectrum.bins.size(); ++index) {
            const Complex64& bin = spectrum.bins[index];
            line(out, text::fixed(static_cast<double>(index) * spectrum.bin_spacing_hz, 6) + " " +
                          text::fixed(level_db(bin, offset), 4) + " " +
                          text::fixed(phase_degrees(bin), 4));
        }
    }

    void operator()(const PowerSpectrumData& power) const {
        // Two columns, not three. REW's importer accepts a missing phase
        // column, and inventing one would be worse than omitting it.
        line(out, "* Freq(Hz) SPL(dB)");
        for (std::size_t index = 0; index < power.magnitude_db.size(); ++index) {
            line(out, text::fixed(static_cast<double>(index) * power.bin_spacing_hz, 6) + " " +
                          text::fixed(power.magnitude_db[index] + offset, 4));
        }
    }

    void operator()(const TransferFunctionData& transfer) const {
        // Coherence is not part of REW's import format, so it rides along as a
        // comment column. A reader that ignores it loses nothing; one that
        // wants it can have it, rather than it being silently discarded.
        line(out, "* Freq(Hz) SPL(dB) Phase(degrees) [coherence]");
        for (std::size_t index = 0; index < transfer.bins.size(); ++index) {
            const Complex64& bin = transfer.bins[index];
            const double gamma =
                index < transfer.coherence.size() ? transfer.coherence[index] : 0.0;
            line(out, text::fixed(static_cast<double>(index) * transfer.bin_spacing_hz, 6) + " " +
                          text::fixed(level_db(bin, offset), 4) + " " +
                          text::fixed(phase_degrees(bin), 4) + " * " + text::fixed(gamma, 4));
        }
    }

    void operator()(const ImpulseResponseData& ir) const {
        // A different shape entirely: time against amplitude, with t = 0 at
        // the recorded arrival rather than at the first sample.
        line(out, "* Time(s) Amplitude");
        line(out, "* Time zero at sample " + text::shortest(ir.time_zero_samples));
        const double rate = measurement.sample_rate > 0.0 ? measurement.sample_rate : 1.0;
        for (std::size_t index = 0; index < ir.samples.size(); ++index) {
            const double seconds = (static_cast<double>(index) - ir.time_zero_samples) / rate;
            line(out, text::fixed(seconds, 9) + " " + text::fixed(ir.samples[index], 9));
        }
    }
};

}  // namespace

std::string to_rew_text(const Measurement& measurement) {
    std::string out;
    const double offset = measurement.references.spl_offset_db.value_or(0.0);
    const bool calibrated = measurement.references.spl_offset_db.has_value();

    std::string name = measurement.name;
    std::ranges::replace(name, '\n', ' ');

    line(out, "* Measurement data saved by analyzer");
    line(out, "* Name: " + name);
    line(out, "* Sample rate: " + text::shortest(measurement.sample_rate) + " Hz");
    if (const auto delay = measurement.references.propagation_delay_seconds) {
        line(out, "* Propagation delay removed: " + text::shortest(*delay) + " s");
    }
    if (calibrated) {
        line(out, "* SPL offset applied: " + text::shortest(offset) + " dB");
    } else {
        line(out, "* Levels are dBFS - this measurement is not SPL calibrated");
    }

    std::visit(Writer{out, measurement, offset}, measurement.data);
    return out;
}

}  // namespace analyzer::model
