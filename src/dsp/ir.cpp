#include "dsp/ir.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <numbers>

#include "base/numeric.hpp"
#include "base/units.hpp"
#include "dsp/complex.hpp"
#include "dsp/fft.hpp"

namespace analyzer::dsp {

namespace {

// Where the response drops into its own noise floor.
//
// A simplified Lundeby: estimate the floor from the last part of the record,
// then find where the energy envelope last stands clear of it. Everything after
// that point is measurement noise rather than the room, and integrating it
// corrupts every reverberation estimate.
//
// Returns the full length when no floor is detectable, which is the right
// answer for a synthetic response that decays to exactly zero.
std::size_t noise_floor_index(std::span<const float> samples) {
    constexpr std::size_t kBlock = 512;
    // Keep integrating until the envelope is within this much of the floor.
    constexpr float kMarginDb = 10.0f;

    if (samples.size() < kBlock * 8) {
        return samples.size();
    }

    std::vector<float> energy;
    energy.reserve(samples.size() / kBlock + 1);
    for (std::size_t begin = 0; begin < samples.size(); begin += kBlock) {
        const auto block = samples.subspan(begin, std::min(kBlock, samples.size() - begin));
        float sum = 0.0f;
        for (const float s : block) {
            sum += s * s;
        }
        energy.push_back(sum / static_cast<float>(block.size()));
    }

    // The floor, taken from the last tenth of the record.
    const std::size_t floor_start = energy.size() - energy.size() / 10;
    const std::size_t floor_count = energy.size() - floor_start;
    if (floor_count == 0) {
        return samples.size();
    }
    float floor = 0.0f;
    for (std::size_t i = floor_start; i < energy.size(); ++i) {
        floor += energy[i];
    }
    floor /= static_cast<float>(floor_count);
    if (floor <= 0.0f) {
        return samples.size();
    }

    const float threshold = floor * db_to_power(kMarginDb);
    for (std::size_t i = energy.size(); i-- > 0;) {
        if (energy[i] > threshold) {
            // One block of headroom past the last clearly-signal block.
            return std::min((i + 2) * kBlock, samples.size());
        }
    }
    // Never clears the floor, so there is nothing to truncate against.
    return samples.size();
}

// Fraction of the decay curve at the end that carries no information.
//
// A backward integral necessarily terminates at negative infinity: the last
// sample integrates only itself, so the curve falls off a cliff regardless of
// what the room did. A truncated measurement therefore *appears* to reach -35 dB
// even when the room only decayed by ten, and a fit that reaches into the cliff
// reports a reverberation time that is pure artefact.
//
// Excluding the tail is the cheap guard. The thorough answer is Lundeby's
// method, which finds where the response meets the noise floor and truncates
// the integration there; that is worth doing later, and this is worth doing now.
constexpr float kTruncationGuard = 0.9f;

// Seconds taken to fall from `from_db` to `to_db`, by least-squares fit.
//
// A fit rather than just reading the two crossings: the crossings alone are at
// the mercy of whatever noise sits on the curve at those two instants, while a
// fit over the whole span uses every point between them.
//
// Returns nullopt if the span is not reached before the truncation guard, which
// means the measurement does not have the range to support this estimate.
std::optional<float> fit_decay(std::span<const float> decay_db, float sample_rate, float from_db,
                               float to_db) {
    if (sample_rate <= 0.0f || decay_db.size() < 4) {
        return std::nullopt;
    }
    const auto usable =
        static_cast<std::size_t>(static_cast<float>(decay_db.size()) * kTruncationGuard);
    const auto searchable = decay_db.first(std::max<std::size_t>(usable, 4));

    const auto crossing = [&](float level_db) {
        return std::find_if(searchable.begin(), searchable.end(),
                            [level_db](float db) { return db <= level_db; });
    };
    const auto start_it = crossing(from_db);
    const auto end_it = crossing(to_db);
    if (start_it == searchable.end() || end_it == searchable.end()) {
        return std::nullopt;
    }
    const auto start = static_cast<std::size_t>(start_it - searchable.begin());
    const auto end = static_cast<std::size_t>(end_it - searchable.begin());
    if (end <= start + 2) {
        return std::nullopt;
    }

    // Least squares over the span, x in samples.
    const auto span = searchable.subspan(start, end - start + 1);
    const auto n = static_cast<double>(span.size());
    double sum_x = 0.0;
    double sum_y = 0.0;
    double sum_xy = 0.0;
    double sum_xx = 0.0;
    for (std::size_t index = 0; index < span.size(); ++index) {
        const auto x = static_cast<double>(index);
        const auto y = static_cast<double>(span[index]);
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_xx += x * x;
    }
    const double denominator = n * sum_xx - sum_x * sum_x;
    if (std::abs(denominator) < 1e-12) {
        return std::nullopt;
    }
    // Decibels per sample; negative for a decay.
    const double slope = (n * sum_xy - sum_x * sum_y) / denominator;
    if (slope >= 0.0) {
        return std::nullopt;
    }

    const auto range = static_cast<double>(from_db - to_db);
    return static_cast<float>(range / -slope / static_cast<double>(sample_rate));
}

// A fitted fall time scaled to the 60 dB it is extrapolating to.
std::optional<float> scaled(std::optional<float> seconds, float factor) noexcept {
    if (!seconds) {
        return std::nullopt;
    }
    return *seconds * factor;
}

}  // namespace

std::vector<float> apply_gate(const ImpulseResponse& ir, const Gate& gate) {
    if (ir.sample_rate <= 0.0f || ir.samples.empty()) {
        return {};
    }

    const auto to_index = [&](float seconds) {
        return std::min(
            saturating_cast<std::size_t>(std::round(ir.peak_samples + seconds * ir.sample_rate)),
            ir.samples.size());
    };
    const std::size_t start = to_index(gate.start_seconds);
    const std::size_t end = to_index(gate.end_seconds);
    if (start >= end) {
        return {};
    }

    const auto kept = std::span<const float>(ir.samples).subspan(start, end - start);
    std::vector<float> out(kept.begin(), kept.end());
    const std::size_t fade =
        std::min(saturating_cast<std::size_t>(gate.fade_seconds * ir.sample_rate), out.size());

    // Taper only the closing edge. The opening edge sits in silence before the
    // arrival, so there is nothing there to discontinuity against.
    if (fade > 1) {
        const std::size_t start_of_fade = out.size() - fade;
        for (std::size_t offset = 0; offset < fade; ++offset) {
            const float t = static_cast<float>(offset) / static_cast<float>(fade - 1);
            // Half a Hann: unity at the start of the fade, zero at the end.
            out[start_of_fade + offset] *= 0.5f * (1.0f + std::cos(std::numbers::pi_v<float> * t));
        }
    }
    return out;
}

std::optional<GatedResponse> gated_response(const ImpulseResponse& ir, const Gate& gate,
                                            std::size_t fft_size) {
    if (fft_size < 2 || fft_size % 2 != 0) {
        return std::nullopt;
    }
    const std::vector<float> gated = apply_gate(ir, gate);
    if (gated.empty()) {
        return std::nullopt;
    }

    RealFft fft(fft_size);
    std::vector<float> padded(fft_size, 0.0f);
    const std::size_t take = std::min(gated.size(), fft_size);
    std::copy_n(gated.begin(), take, padded.begin());

    std::vector<Complex32> spectrum(fft.bins());
    fft.forward(padded, spectrum);

    GatedResponse response;
    response.magnitude_db.reserve(spectrum.size());
    response.phase_degrees.reserve(spectrum.size());
    for (const Complex32 bin : spectrum) {
        const float magnitude = std::abs(bin);
        response.magnitude_db.push_back(magnitude > 0.0f ? amplitude_to_db(magnitude) : kIrFloorDb);
        response.phase_degrees.push_back(std::arg(bin) * kDegreesPerRadian<float>);
    }
    response.bin_spacing_hz = ir.sample_rate / static_cast<float>(fft_size);
    response.resolution_hz = gate.resolution_hz();
    return response;
}

std::vector<float> schroeder_decay(const ImpulseResponse& ir) {
    const std::size_t start =
        std::min(saturating_cast<std::size_t>(ir.peak_samples), ir.samples.size());
    const auto full_tail = std::span<const float>(ir.samples).subspan(start);
    if (full_tail.empty()) {
        return {};
    }

    // Truncate at the noise floor before integrating.
    //
    // Integrating a long noise floor is not harmless. The floor's total energy
    // can rival or exceed the decay's, which holds the curve near 0 dB right
    // through the part that matters and then declines slowly for seconds. A fit
    // spanning that reports a reverberation time of minutes. Measured on the
    // synthetic room this module is tested against, EDT came out at 1083
    // seconds before this truncation and 0.5 after it.
    const std::size_t usable = noise_floor_index(full_tail);
    const auto tail = full_tail.first(usable);
    if (tail.empty()) {
        return {};
    }

    // Reverse cumulative sum of energy.
    std::vector<float> curve(tail.size(), 0.0f);
    double running = 0.0;
    for (std::size_t i = tail.size(); i-- > 0;) {
        running += static_cast<double>(tail[i]) * static_cast<double>(tail[i]);
        curve[i] = static_cast<float>(running);
    }

    const float total = curve.front();
    if (total <= 0.0f) {
        return std::vector<float>(curve.size(), kIrFloorDb);
    }
    for (float& value : curve) {
        value = power_to_db(value / total, kIrFloorDb);
    }
    return curve;
}

std::optional<float> ReverbTime::best() const noexcept {
    if (t30) {
        return t30;
    }
    if (t20) {
        return t20;
    }
    return edt;
}

std::optional<float> ReverbTime::spread() const noexcept {
    std::array<float, 3> values{};
    std::size_t count = 0;
    for (const std::optional<float>& estimate : {edt, t20, t30}) {
        if (estimate) {
            values[count++] = *estimate;
        }
    }
    if (count < 2) {
        return std::nullopt;
    }
    const auto [min, max] = std::minmax_element(values.begin(), values.begin() + count);
    if (*max > 0.0f) {
        return (*max - *min) / *max;
    }
    return std::nullopt;
}

ReverbTime reverb_time(std::span<const float> decay_db, float sample_rate) {
    return {
        .edt = scaled(fit_decay(decay_db, sample_rate, 0.0f, -10.0f), 6.0f),
        .t20 = scaled(fit_decay(decay_db, sample_rate, -5.0f, -25.0f), 3.0f),
        .t30 = scaled(fit_decay(decay_db, sample_rate, -5.0f, -35.0f), 2.0f),
    };
}

WindowKind recommended_gate_window() {
    // The gate itself does the tapering, so a second window would narrow the
    // effective gate and cost resolution for nothing.
    return WindowKind::rectangular();
}

std::optional<Window> gate_window(const Gate& gate, float sample_rate) {
    const std::size_t length = saturating_cast<std::size_t>(gate.length_seconds() * sample_rate);
    if (length == 0) {
        return std::nullopt;
    }
    const float alpha = gate.length_seconds() > 0.0f
                            ? std::clamp(gate.fade_seconds / gate.length_seconds(), 0.0f, 1.0f)
                            : 0.0f;
    return Window(WindowKind::tukey(alpha), length);
}

}  // namespace analyzer::dsp
