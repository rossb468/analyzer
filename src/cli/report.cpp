#include "cli/report.hpp"

#include "base/lines.hpp"
#include "base/number_text.hpp"
#include "base/peak.hpp"
#include "cli/text.hpp"

namespace analyzer::cli {

namespace {

using text::append_line;

}  // namespace

std::string describe(dsp::WindowKind window) {
    std::string name = dsp::to_string(window.shape);
    if (window.shape == dsp::WindowKind::Shape::Tukey) {
        name += " { alpha: " + debug_float(window.tukey_alpha) + " }";
    }
    return name;
}

std::string describe(dsp::Averaging averaging) {
    using Mode = dsp::Averaging::Mode;
    switch (averaging.mode) {
        case Mode::None: return "None";
        case Mode::Exponential:
            return "Exponential { alpha: " + debug_float(averaging.alpha) + " }";
        case Mode::Linear: return "Linear { frames: " + std::to_string(averaging.frames) + " }";
        case Mode::Infinite: return "Infinite";
        case Mode::PeakHold: return "PeakHold";
    }
    return "unknown";
}

std::string render(const engine::SpectrumFrame& frame, const Meta& meta,
                   std::optional<float> min_db, bool peak_only) {
    std::string out;
    out.reserve(frame.bins.size() * 24 + 640);

    append_line(out, "# analyzer-cli spectrum");
    append_line(out, "# source: " + meta.source);
    append_line(out, "# sample rate: " + text::shortest(meta.sample_rate) + " Hz");
    append_line(out, "# channels: " + std::to_string(meta.channels) + " (analysed channel " +
                         std::to_string(meta.channel) + ")");
    append_line(out, "# fft size: " + std::to_string(meta.fft_size));
    append_line(out, "# window: " + describe(meta.window) + ", ENBW " +
                         text::fixed(meta.enbw_hz, 4) + " Hz");
    append_line(out, "# overlap: " + text::fixed(dsp::overlap_fraction(meta.overlap) * 100.0f, 1) +
                         "%, hop " + std::to_string(meta.hop) + " frames");
    append_line(out, "# averaging: " + describe(meta.averaging));
    append_line(out, "# frames averaged: " + std::to_string(frame.frames_averaged));
    append_line(out, "# bin spacing: " + text::fixed(frame.bin_spacing_hz, 6) + " Hz");
    if (frame.overruns > 0) {
        append_line(out, "# WARNING: " + std::to_string(frame.overruns) + " dropped block(s)");
    }
    append_line(out, "# level reference: 0 dBFS = full-scale sine");
    append_line(out, "# frequency_hz\tlevel_db");

    const auto row = [&](std::size_t bin, float level) {
        append_line(out, text::fixed(frame.bin_frequency(bin), 6) + "\t" + text::fixed(level, 4));
    };

    if (peak_only) {
        // The last of several equal maxima, so a spectrum that is all silence
        // reports its top bin; see base/peak.hpp.
        if (!frame.bins.empty()) {
            const std::size_t loudest = last_max_index(frame.bins);
            row(loudest, frame.bins[loudest]);
        }
        return out;
    }

    for (std::size_t bin = 0; bin < frame.bins.size(); ++bin) {
        if (min_db && frame.bins[bin] < *min_db) {
            continue;
        }
        row(bin, frame.bins[bin]);
    }
    return out;
}

}  // namespace analyzer::cli
