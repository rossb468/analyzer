// Multi-time-window transfer function analysis.
//
// A single FFT forces one choice of resolution for the whole spectrum, and
// there is no good answer. Pick 32768 points for 1.5 Hz in the bass and the
// frame spans 680 ms, so the display lags badly and the top octave gets
// thousands of bins nobody needs. Pick 1024 for a responsive top end and the
// bass is 47 Hz per bin, which cannot resolve a room mode at all.
//
// MTW runs several transfer function engines at once, each with its own FFT
// size, and splices their outputs - a long window where frequencies are close
// together and change slowly, a short one where they are far apart and change
// fast. That is the technique Smaart is built around, and it is the main thing
// this offers over REW's real-time side.
//
// Why the splices line up
//
// It is not obvious that independently windowed engines should agree on phase.
// They do, and the reason is worth stating: the transfer function is a *ratio*.
// Shifting the analysis window shifts both X and Y by the same amount,
// multiplying both by the same e^(-j*w*tau), which cancels in Y/X. Each engine
// is therefore self-consistent, and all of them are consistent with each other,
// provided each one sees its two channels sample-aligned.
//
// Magnitude and coherence line up for the same reason.
//
// What does not line up
//
// Coherence still means something slightly different in each band, because it
// is measured over a different window length. A band using a 256-point window
// is asking "are these correlated over 5 ms", and one using 32768 points is
// asking about 680 ms. Both are useful, neither is wrong, and no amount of
// splicing makes them the same question. This is documented rather than papered
// over.

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "dsp/complex.hpp"
#include "dsp/spectrum.hpp"
#include "dsp/transfer.hpp"
#include "dsp/window.hpp"

namespace analyzer::dsp {

// Floor for a point with no usable reference energy.
inline constexpr float kMtwFloorDb = -200.0f;

// One spliced output point.
struct MtwPoint {
    // Frequency in hertz.
    float hz = 0.0f;
    // Magnitude in decibels.
    float magnitude_db = 0.0f;
    // Phase in degrees, wrapped to -180..180.
    float phase_degrees = 0.0f;
    // Coherence, 0..1. Comparable within a band, only roughly across bands.
    float coherence = 0.0f;
    // FFT size of the band this point came from, for display and diagnosis.
    std::size_t fft_size = 0;

    friend constexpr bool operator==(const MtwPoint&, const MtwPoint&) = default;
};

// Setup for MultiTimeWindow.
struct MtwConfig {
    // Sample rate in hertz.
    float sample_rate = 48'000.0f;
    // FFT size for the lowest band. Larger means finer bass resolution and more
    // latency: 32768 gives 1.5 Hz and 680 ms at 48 kHz, 65536 gives 0.73 Hz and
    // 1.4 s.
    std::size_t largest_fft = 32'768;
    // FFT size for the highest band.
    std::size_t smallest_fft = 256;
    // Analysis window, used by every band.
    WindowKind window = WindowKind::hann();
    // Frame overlap, used by every band.
    Overlap overlap = Overlap::Half;
    // Averaging mode, used by every band.
    TransferAveraging averaging = TransferAveraging::exponential(0.2f);
    // Output points per octave. 48 is plenty for a display and keeps the total
    // in the hundreds rather than the tens of thousands.
    std::size_t points_per_octave = 48;
    // Lowest output frequency.
    float min_hz = 20.0f;
};

// Spliced multi-resolution transfer function.
//
// Owned by the analysis thread; not safe to share without external
// synchronisation. Everything is allocated at construction, so push() and
// resolve() do not allocate.
class MultiTimeWindow {
public:
    // Build the band engines and the output grid.
    //
    // The FFT sizes must be powers of two with 64 <= smallest_fft <=
    // largest_fft, and the sample rate, grid density and lowest frequency must
    // be positive.
    explicit MultiTimeWindow(const MtwConfig& config);

    // Feed matched samples to every band.
    //
    // Returns the number of frames the *lowest* band completed, since that is
    // the one that gates a full-bandwidth result.
    //
    // The spans must be the same length.
    std::size_t push(std::span<const float> reference, std::span<const float> measurement) noexcept;

    // Recompute the spliced output.
    //
    // Call before reading points(); it is separate from push() so a display
    // running at 120 Hz does not force a resplice on every audio block.
    void resolve() noexcept;

    // The spliced result. Call resolve() first.
    std::span<const MtwPoint> points() const noexcept { return points_; }

    // Frames the lowest band has averaged, which gates a trustworthy bass
    // reading.
    std::uint32_t frames() const noexcept { return frames_; }

    // FFT sizes in use, from the highest band down to the lowest. Allocates;
    // not for the analysis thread.
    std::vector<std::size_t> fft_sizes() const;

    // Bin spacing of the lowest band - the finest resolution available.
    float finest_resolution_hz() const noexcept;

    // Seconds of audio the lowest band's window spans, which is the latency
    // before the bass reading settles.
    float longest_window_seconds() const noexcept;

    // Discard all averages.
    void reset() noexcept;

private:
    // One band: an engine plus the frequency range it is responsible for.
    struct Band {
        TransferFunction engine;
        std::vector<Complex32> response;
        std::vector<float> coherence;
        float lower_hz;
        float upper_hz;
    };

    float sample_rate_;
    std::vector<Band> bands_;
    std::vector<MtwPoint> points_;
    std::uint32_t frames_ = 0;
};

}  // namespace analyzer::dsp
