// The equaliser as the audio thread sees it.
//
// The UI thread owns the Equaliser: bands, gains, the trim. The audio thread
// cannot touch it, because it owns std::vectors and the audio thread must never
// meet an allocation. So the UI thread boils it down to EqCoefficients - a
// plain array - and publishes that through a triple buffer, and the audio
// thread runs its own biquad sections from the newest one it has seen.

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "dsp/biquad.hpp"
#include "dsp/eq.hpp"
#include "engine/snapshot.hpp"
#include "ffi/internal.hpp"

namespace analyzer::ffi {

// Coefficients handed to the audio thread.
//
// A plain array rather than the Equaliser itself: the equaliser owns a vector,
// and the audio thread must never touch an allocation. `generation` lets the
// callback tell a change from a re-read, so it only copies coefficients when
// they actually moved - and it copies coefficients only, leaving each section's
// delay line alone so a fader move does not click.
struct EqCoefficients {
    std::uint64_t generation = 0;
    std::size_t count = 0;
    std::array<dsp::Biquad, kMaxEqBands> sections{};
    float trim = 1.0f;

    // Design every band of `eq` at its sample rate. Bands beyond the array are
    // dropped; the entry points refuse to add them, so this is the backstop.
    static EqCoefficients from_equaliser(std::uint64_t generation, const dsp::Equaliser& eq);
};

// Owned by the audio callback. The filter state lives here, on the audio
// thread, because it belongs to the running filter rather than to the settings.
class EqProcessor {
public:
    explicit EqProcessor(engine::SnapshotReader<EqCoefficients> reader)
        : reader_(std::move(reader)) {}

    // Filter `samples` in place through whatever was last published.
    void process(std::span<float> samples) noexcept;

private:
    engine::SnapshotReader<EqCoefficients> reader_;
    std::array<dsp::Biquad, kMaxEqBands> sections_{};
    std::size_t count_ = 0;
    float trim_ = 1.0f;
    std::uint64_t generation_ = 0;
};

}  // namespace analyzer::ffi
