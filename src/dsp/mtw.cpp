#include "dsp/mtw.hpp"

#include <algorithm>
#include <bit>
#include <cmath>

#include "base/contract.hpp"
#include "base/numeric.hpp"
#include "base/units.hpp"

namespace analyzer::dsp {

namespace {

// x mod 360 in [0, 360), the sign of the result following the divisor rather
// than the dividend as std::fmod would have it.
float wrap_degrees_positive(float degrees) noexcept {
    const float r = std::fmod(degrees, 360.0f);
    return (r < 0.0f) ? r + 360.0f : r;
}

}  // namespace

MultiTimeWindow::MultiTimeWindow(const MtwConfig& config) : sample_rate_(config.sample_rate) {
    ANALYZER_EXPECTS(config.sample_rate > 0.0f, "sample rate must be positive");
    ANALYZER_EXPECTS(
        std::has_single_bit(config.smallest_fft) && std::has_single_bit(config.largest_fft),
        "FFT sizes must be powers of two");
    ANALYZER_EXPECTS(config.smallest_fft >= 64 && config.smallest_fft <= config.largest_fft,
                     "need 64 <= smallest_fft <= largest_fft");
    ANALYZER_EXPECTS(config.points_per_octave > 0, "points per octave must be at least 1");
    ANALYZER_EXPECTS(config.min_hz > 0.0f, "min_hz must be positive");

    const float nyquist = config.sample_rate / 2.0f;

    // The top band takes the top octave with the shortest window; each octave
    // down doubles the FFT size, which keeps the number of bins per octave
    // roughly constant instead of piling them up at the top.
    std::size_t size = config.smallest_fft;
    float upper = nyquist;

    for (;;) {
        const bool is_last = size >= config.largest_fft;
        // The lowest band runs all the way down to DC; it has the resolution
        // to, and nothing below it would otherwise be covered.
        const float lower = is_last ? 0.0f : upper / 2.0f;

        bands_.push_back(Band{
            TransferFunction(TransferConfig{
                .sample_rate = config.sample_rate,
                .size = size,
                .window = config.window,
                .overlap = config.overlap,
                .averaging = config.averaging,
            }),
            std::vector<Complex32>(size / 2 + 1),
            std::vector<float>(size / 2 + 1, 0.0f),
            lower,
            upper,
        });

        if (is_last) {
            break;
        }
        upper = lower;
        size *= 2;
    }

    // Log-spaced output grid. This is where the point count collapses: a
    // 32768-point FFT has 16385 bins, and 48 per octave across the audio band is
    // a few hundred.
    const float octaves = std::log2(nyquist / config.min_hz);
    const auto per_octave = static_cast<float>(config.points_per_octave);
    const auto count = saturating_cast<std::size_t>(std::ceil(octaves * per_octave));
    const float step = std::pow(2.0f, 1.0f / per_octave);

    points_.reserve(count + 1);
    float hz = config.min_hz;
    while (hz <= nyquist && points_.size() <= count + 1) {
        points_.push_back(MtwPoint{
            .hz = hz,
            .magnitude_db = kMtwFloorDb,
            .phase_degrees = 0.0f,
            .coherence = 0.0f,
            .fft_size = 0,
        });
        hz *= step;
    }
}

std::size_t MultiTimeWindow::push(std::span<const float> reference,
                                  std::span<const float> measurement) noexcept {
    ANALYZER_EXPECTS(reference.size() == measurement.size(),
                     "reference and measurement must be sample aligned");
    std::size_t slowest = 0;
    for (auto& band : bands_) {
        // The last band is the largest FFT and the slowest to fill, so its
        // count is the one returned.
        slowest = band.engine.push(reference, measurement);
    }
    frames_ = bands_.back().engine.frames();
    return slowest;
}

void MultiTimeWindow::resolve() noexcept {
    for (auto& band : bands_) {
        band.engine.write_response(band.response);
        band.engine.write_coherence(band.coherence);
    }

    for (auto& point : points_) {
        const auto found = std::find_if(bands_.begin(), bands_.end(), [&point](const Band& band) {
            return point.hz >= band.lower_hz && point.hz < band.upper_hz;
        });
        if (found == bands_.end()) {
            continue;
        }
        const Band& band = *found;

        const float spacing = band.engine.bin_spacing_hz();
        if (spacing <= 0.0f) {
            continue;
        }
        const float exact = point.hz / spacing;
        const auto lower = saturating_cast<std::size_t>(std::floor(exact));
        const float fraction = exact - static_cast<float>(lower);

        // The bin above may not exist at the very top of the lowest band's
        // range; fall back to the bin below rather than reading past the end.
        const Complex32 a = lower < band.response.size() ? band.response[lower] : Complex32{};
        const Complex32 b = lower + 1 < band.response.size() ? band.response[lower + 1] : a;

        // Interpolate magnitude and angle separately, taking the shortest
        // angular path. Two obvious alternatives are both wrong:
        //
        // - Blending phase as a plain number breaks across a +-180 degree wrap
        //   and sends the result the long way round.
        // - Blending the complex values linearly chords across the arc, so
        //   magnitude sags wherever phase rotates fast between bins. With a
        //   200-sample delay that is a 35 degree step and a 0.6 dB dip - small,
        //   but a systematic error in the magnitude curve.
        //
        // arg(b * conj(a)) is the signed angle from a to b already wrapped into
        // (-pi, pi], which is exactly the shortest path. It is still wrong if
        // the true step exceeds 180 degrees, but that is aliasing in the
        // underlying delay and no interpolation recovers from it.
        const float magnitude_a = std::abs(a);
        const float magnitude_b = std::abs(b);
        const float magnitude = magnitude_a + (magnitude_b - magnitude_a) * fraction;
        const float step = std::arg(b * std::conj(a));
        const float angle = std::arg(a) + step * fraction;

        const float ca = lower < band.coherence.size() ? band.coherence[lower] : 0.0f;
        const float cb = lower + 1 < band.coherence.size() ? band.coherence[lower + 1] : ca;

        point.magnitude_db = amplitude_to_db(magnitude, kMtwFloorDb);
        // Re-wrap, since arg(a) + step can leave the principal range.
        point.phase_degrees =
            wrap_degrees_positive(angle * kDegreesPerRadian<float> + 180.0f) - 180.0f;
        point.coherence = std::clamp(ca + (cb - ca) * fraction, 0.0f, 1.0f);
        point.fft_size = band.engine.size();
    }
}

std::vector<std::size_t> MultiTimeWindow::fft_sizes() const {
    std::vector<std::size_t> sizes;
    sizes.reserve(bands_.size());
    for (const auto& band : bands_) {
        sizes.push_back(band.engine.size());
    }
    return sizes;
}

float MultiTimeWindow::finest_resolution_hz() const noexcept {
    return bands_.empty() ? 0.0f : bands_.back().engine.bin_spacing_hz();
}

float MultiTimeWindow::longest_window_seconds() const noexcept {
    return bands_.empty() ? 0.0f : static_cast<float>(bands_.back().engine.size()) / sample_rate_;
}

void MultiTimeWindow::reset() noexcept {
    for (auto& band : bands_) {
        band.engine.reset();
    }
    frames_ = 0;
    for (auto& point : points_) {
        point.magnitude_db = kMtwFloorDb;
        point.phase_degrees = 0.0f;
        point.coherence = 0.0f;
    }
}

}  // namespace analyzer::dsp
