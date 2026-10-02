#include "cli/measure.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <optional>
#include <utility>
#include <vector>

#include "base/lines.hpp"
#include "base/number_text.hpp"
#include "base/numeric.hpp"
#include "cli/error.hpp"
#include "cli/source.hpp"
#include "dsp/deconv.hpp"
#include "dsp/generator.hpp"
#include "dsp/ir.hpp"

namespace analyzer::cli {

namespace {

// Speed of sound used to turn a delay into a distance.
constexpr float kSpeedOfSound = 343.0f;

using text::append_line;

// The synthetic room the demo measures.
//
// Deliberately simple and exactly known, so the report can print the truth
// beside the measurement and any discrepancy is visible rather than plausible.
struct SyntheticRoom {
    struct Reflection {
        // Seconds after the direct arrival.
        float offset_seconds;
        // Amplitude relative to the direct arrival.
        float gain;
    };

    explicit SyntheticRoom(float rate) : sample_rate(rate) {}

    float sample_rate;
    // Direct arrival in seconds. 3.43 m, a plausible listening distance.
    float direct_seconds = 0.010f;
    // Discrete reflections.
    std::array<Reflection, 3> reflections{{{0.006f, 0.5f}, {0.011f, 0.35f}, {0.018f, 0.25f}}};
    // Reverberation time built into the tail.
    float rt60 = 0.45f;

    // Convolve a stimulus with this room.
    //
    // The reverberant tail is built from sparse taps - a few thousand delayed,
    // exponentially decaying copies scattered through the tail - rather than a
    // dense impulse response. A dense convolution of a one second sweep with a
    // one second tail is billions of operations for a demo, and sparse taps give
    // the genuine exponential *energy* decay that a reverberation measurement
    // actually reads.
    //
    // Adding a decaying noise burst straight to the response, which is the
    // obvious shortcut, does not work: the tail then is not a filtered version
    // of the stimulus, so deconvolution cannot recover it and reverberation
    // reads about five times short.
    std::vector<float> respond(std::span<const float> stimulus, float tail_seconds) const {
        constexpr std::size_t kTailTaps = 1200;

        const auto tail = saturating_cast<std::size_t>(tail_seconds * sample_rate);
        const auto direct = saturating_cast<std::size_t>(direct_seconds * sample_rate);
        std::vector<float> out(stimulus.size() + direct + tail + 1, 0.0f);

        std::vector<std::pair<std::size_t, float>> arrivals{{direct, 1.0f}};
        for (const Reflection& reflection : reflections) {
            arrivals.emplace_back(
                direct + saturating_cast<std::size_t>(reflection.offset_seconds * sample_rate),
                reflection.gain);
        }

        // Sparse diffuse tail, amplitude falling as exp(-t/tau) so energy falls
        // at 60 dB over rt60 seconds.
        const float tau = rt60 / 6.908f;
        dsp::Generator generator(sample_rate, dsp::Signal::white_noise(1.0f), 99);
        std::vector<float> jitter(kTailTaps * 2, 0.0f);
        generator.fill(jitter);

        for (std::size_t tap = 0; tap < kTailTaps; ++tap) {
            // Spread the taps across the tail, nudged so they are not periodic -
            // regular spacing would produce comb filtering rather than diffusion.
            const float base = (static_cast<float>(tap) / static_cast<float>(kTailTaps)) *
                               static_cast<float>(tail);
            const float nudge = jitter[tap * 2] * (static_cast<float>(tail) * 0.0005f);
            const auto offset = saturating_cast<std::size_t>(std::max(base + nudge, 0.0f));
            const float t = static_cast<float>(offset) / sample_rate;
            const float sign = jitter[tap * 2 + 1] < 0.0f ? -1.0f : 1.0f;
            // Scaled so the whole tail sits below the direct arrival.
            const float gain = sign * 0.06f * std::exp(-t / tau);
            arrivals.emplace_back(direct + offset, gain);
        }

        for (const auto& [delay, gain] : arrivals) {
            if (std::abs(gain) < 1e-6f || delay >= out.size()) {
                continue;
            }
            const std::size_t count = std::min(stimulus.size(), out.size() - delay);
            for (std::size_t index = 0; index < count; ++index) {
                out[index + delay] += stimulus[index] * gain;
            }
        }
        return out;
    }
};

void report_impulse(std::string& out, const dsp::ImpulseResponse& ir) {
    const float arrival_seconds = ir.peak_samples / ir.sample_rate;
    append_line(out, "#   direct arrival     " + text::fixed(arrival_seconds * 1000.0f, 2) +
                         " ms (" + text::fixed(arrival_seconds * kSpeedOfSound, 2) + " m)");
    append_line(out, "#   impulse length     " + std::to_string(ir.samples.size()) + " samples (" +
                         text::fixed(ir.duration_seconds(), 3) + " s)");

    // Discrete arrivals standing clear of the local background, which is what a
    // reflection looks like in an impulse response.
    const float peak = ir.peak_amplitude();
    const auto start = saturating_cast<std::size_t>(ir.peak_samples);
    const auto window = saturating_cast<std::size_t>(0.030f * ir.sample_rate);
    int found = 0;
    const std::size_t stop = std::min(start + window, ir.samples.size());
    for (std::size_t index = start + 8; index < stop; ++index) {
        const float magnitude = std::abs(ir.samples[index]);
        if (magnitude < peak * 0.15f) {
            continue;
        }
        const bool is_local_peak = index >= 1 && std::abs(ir.samples[index - 1]) < magnitude &&
                                   index + 1 < ir.samples.size() &&
                                   std::abs(ir.samples[index + 1]) <= magnitude;
        if (!is_local_peak) {
            continue;
        }
        append_line(out, "#   reflection         +" +
                             text::fixed((static_cast<float>(index) - ir.peak_samples) /
                                             ir.sample_rate * 1000.0f,
                                         2) +
                             " ms at " + text::fixed(magnitude / peak * 100.0f, 0) + "%");
        ++found;
        if (found >= 6) {
            break;
        }
    }
}

void report_reverberation(std::string& out, const dsp::ImpulseResponse& ir) {
    const std::vector<float> decay = dsp::schroeder_decay(ir);
    const dsp::ReverbTime rt = dsp::reverb_time(decay, ir.sample_rate);

    const auto show = [](const std::string& label, std::optional<float> value) {
        return value ? label + " " + text::fixed(*value, 3) + " s"
                     : label + " (insufficient range)";
    };
    append_line(out, "#   " + show("EDT               ", rt.edt));
    append_line(out, "#   " + show("T20               ", rt.t20));
    append_line(out, "#   " + show("T30               ", rt.t30));

    if (const auto spread = rt.spread()) {
        const char* verdict =
            *spread < 0.1f ? "consistent" : "estimates disagree - the decay is not a straight line";
        append_line(out, "#   agreement          " + text::fixed(*spread * 100.0f, 1) + "% (" +
                             verdict + ")");
    }
}

void report_response(std::string& out, const dsp::ImpulseResponse& ir,
                     const MeasureOptions& options) {
    const dsp::Gate gate = dsp::Gate::anechoic(options.gate_ms / 1000.0f);
    const auto response = dsp::gated_response(ir, gate, options.fft);
    if (!response) {
        throw CliError("the gate kept no samples - is it shorter than the arrival?");
    }

    append_line(out, "#");
    append_line(out, "# gated response: " + text::fixed(gate.length_seconds() * 1000.0f, 1) +
                         " ms window, valid above " + text::fixed(response->resolution_hz, 0) +
                         " Hz");
    append_line(out, "# frequency_hz\tlevel_db\ttrustworthy");

    // Log-spaced rows, because a linear listing of 2048 bins helps nobody.
    const float lowest = 20.0f;
    const float highest = std::min(ir.sample_rate / 2.0f, 20'000.0f);
    const std::size_t steps = std::max<std::size_t>(options.response_rows, 2);
    const float ratio = std::pow(highest / lowest, 1.0f / static_cast<float>(steps - 1));

    float hz = lowest;
    for (std::size_t step = 0; step < steps; ++step) {
        const auto bin = saturating_cast<std::size_t>(std::round(hz / response->bin_spacing_hz));
        if (bin < response->magnitude_db.size()) {
            append_line(out, text::fixed(hz, 1) + "\t" +
                                 text::fixed(response->magnitude_db[bin], 2) + "\t" +
                                 (response->is_trustworthy(hz) ? "yes" : "no"));
        }
        hz *= ratio;
    }
}

}  // namespace

std::string demo(const MeasureOptions& options) {
    const float sample_rate = 48'000.0f;
    const SyntheticRoom room(sample_rate);

    const auto samples = saturating_cast<std::size_t>(sample_rate * options.seconds);
    dsp::Generator generator(sample_rate,
                             dsp::Signal::sweep(20.0f, 20'000.0f, options.seconds, 0.5f, false), 1);
    std::vector<float> stimulus(samples, 0.0f);
    generator.fill(stimulus);

    const std::vector<float> response = room.respond(stimulus, room.rt60 * 3.0f);

    std::string out;
    append_line(out, "# synthetic room measurement");
    append_line(out, "#");
    append_line(out, "# constructed:");
    append_line(out, "#   direct arrival     " + text::fixed(room.direct_seconds * 1000.0f, 2) +
                         " ms (" + text::fixed(room.direct_seconds * kSpeedOfSound, 2) + " m)");
    for (const auto& reflection : room.reflections) {
        append_line(out, "#   reflection         +" +
                             text::fixed(reflection.offset_seconds * 1000.0f, 2) + " ms at " +
                             text::fixed(reflection.gain * 100.0f, 0) + "%");
    }
    append_line(out, "#   reverberation      " + text::fixed(room.rt60, 3) + " s");
    append_line(out, "#");

    out += analyse(stimulus, response, sample_rate, options);
    return out;
}

std::string analyse(std::span<const float> stimulus, std::span<const float> response,
                    float sample_rate, const MeasureOptions& options) {
    const std::size_t longest = std::max(stimulus.size(), response.size());
    dsp::Deconvolver deconvolver(sample_rate, longest);
    const auto ir = deconvolver.deconvolve(stimulus, response, dsp::kDefaultRegularisation);
    if (!ir) {
        throw CliError("deconvolution failed - is either signal silent?");
    }

    std::string out;
    append_line(out, "# measured:");
    report_impulse(out, *ir);
    report_reverberation(out, *ir);
    report_response(out, *ir, options);
    return out;
}

std::string from_files(const std::filesystem::path& stimulus_path,
                       const std::filesystem::path& response_path, const MeasureOptions& options) {
    const audio::Source stimulus = read_source(stimulus_path);
    const audio::Source response = read_source(response_path);

    if (std::abs(stimulus.sample_rate - response.sample_rate) > 0.5) {
        throw CliError("sample rate mismatch: stimulus is " + text::shortest(stimulus.sample_rate) +
                       " Hz, response is " + text::shortest(response.sample_rate) + " Hz");
    }

    // Both files are reduced to their first channel. A stimulus is
    // single-channel by nature, and a multi-channel recording needs an explicit
    // choice rather than a silent mixdown.
    const auto take_first = [](const audio::Source& source) {
        std::vector<float> first;
        first.reserve(source.frames());
        for (std::size_t i = 0; i < source.samples.size(); i += source.channels) {
            first.push_back(source.samples[i]);
        }
        return first;
    };

    std::string out;
    append_line(out, "# swept measurement");
    append_line(out, "#   stimulus  " + stimulus_path.string());
    append_line(out, "#   response  " + response_path.string());
    append_line(out, "#   rate      " + text::shortest(stimulus.sample_rate) + " Hz");
    append_line(out, "#");

    out += analyse(take_first(stimulus), take_first(response),
                   static_cast<float>(stimulus.sample_rate), options);
    return out;
}

}  // namespace analyzer::cli
