#include "cli/report.hpp"

#include <compare>

#include "base/number_text.hpp"
#include "cli/text.hpp"

namespace analyzer::cli {

namespace {

void line(std::string& out, const std::string& text) {
    out += text;
    out += '\n';
}

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

    line(out, "# analyzer-cli spectrum");
    line(out, "# source: " + meta.source);
    line(out, "# sample rate: " + text::shortest(meta.sample_rate) + " Hz");
    line(out, "# channels: " + std::to_string(meta.channels) + " (analysed channel " +
                  std::to_string(meta.channel) + ")");
    line(out, "# fft size: " + std::to_string(meta.fft_size));
    line(out,
         "# window: " + describe(meta.window) + ", ENBW " + text::fixed(meta.enbw_hz, 4) + " Hz");
    line(out, "# overlap: " + text::fixed(dsp::overlap_fraction(meta.overlap) * 100.0f, 1) +
                  "%, hop " + std::to_string(meta.hop) + " frames");
    line(out, "# averaging: " + describe(meta.averaging));
    line(out, "# frames averaged: " + std::to_string(frame.frames_averaged));
    line(out, "# bin spacing: " + text::fixed(frame.bin_spacing_hz, 6) + " Hz");
    if (frame.overruns > 0) {
        line(out, "# WARNING: " + std::to_string(frame.overruns) + " dropped block(s)");
    }
    line(out, "# level reference: 0 dBFS = full-scale sine");
    line(out, "# frequency_hz\tlevel_db");

    const auto row = [&](std::size_t bin, float level) {
        line(out, text::fixed(frame.bin_frequency(bin), 6) + "\t" + text::fixed(level, 4));
    };

    if (peak_only) {
        // Rust's max_by keeps the last of several equal maxima, and compares
        // with total_cmp, which orders -0.0 below 0.0 and NaN above everything.
        // Both matter when a whole spectrum is silence: the answer is the top bin.
        if (!frame.bins.empty()) {
            std::size_t loudest = 0;
            for (std::size_t bin = 1; bin < frame.bins.size(); ++bin) {
                if (std::strong_order(frame.bins[bin], frame.bins[loudest]) >= 0) {
                    loudest = bin;
                }
            }
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
