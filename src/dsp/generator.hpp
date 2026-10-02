// Stimulus generation.
//
// A spectrum tells you what came out. To learn what a system *did* you have to
// know what went in, which means generating it. Everything here is real-time
// safe: Generator::fill() allocates nothing and takes no locks, because it runs
// inside the audio callback alongside capture.
//
// Why the phase accumulator is a double
//
// A float phase accumulator drifts. It has 24 bits of mantissa, so once the
// accumulated phase passes a few million radians the increment stops being
// representable and the generated frequency quietly shifts. At 48 kHz that is
// minutes, not hours - well inside a measurement session.
//
// Why the noise generator is our own
//
// Not <random>. The standard distributions are not specified bit for bit, so
// the same seed gives different noise on libstdc++, libc++ and MSVC, and a
// test that passes on one platform could flake on another. A 64-bit xorshift
// is a handful of lines, never blocks, and gives every platform the same
// stream.

#pragma once

#include <cstdint>
#include <span>

namespace analyzer::dsp {

// The noise seed for every run that has to be reproducible: generated files,
// swept measurements, the live stimulus.
//
// Two runs of the same command must produce the same bytes, or a parity
// comparison cannot be repeated and a noise measurement cannot be compared
// against itself.
inline constexpr std::uint64_t kDefaultSeed = 0x5EED'5EED'5EED'5EEDull;

// What to generate.
//
// A small value type rather than a bare enum because the shapes carry
// parameters. Every field is a scalar, so it is trivially copyable and can be
// handed to the audio thread by value. Only the fields named for a kind are
// read; build one with the named constructors:
//
//   Signal::sine(997.0f, 0.5f)
//   Signal::sweep(20.0f, 20000.0f, 5.0f, 0.5f, false)
struct Signal {
    enum class Kind {
        // Digital silence.
        Silence,
        // A steady sine. The stimulus for calibration and distortion work.
        // Uses hz and amplitude.
        Sine,
        // Equal energy per hertz. Sounds bright, because each octave up holds
        // twice the bandwidth and so twice the power. Uses amplitude.
        WhiteNoise,
        // Equal energy per octave, falling at 3 dB per octave. The usual
        // stimulus for transfer function work, because it puts comparable
        // energy in every part of a log frequency display. Uses amplitude.
        PinkNoise,
        // Exponential sine sweep, the Farina stimulus.
        //
        // Spends equal time per octave and lets harmonic distortion be
        // separated from the linear response after deconvolution, which a
        // linear sweep cannot do. This is what swept measurement will use. Uses
        // start_hz, end_hz, seconds, amplitude and repeat.
        Sweep,
    };

    Kind kind = Kind::Silence;
    // Sine: frequency in hertz.
    float hz = 0.0f;
    // Sweep: starting frequency in hertz.
    float start_hz = 0.0f;
    // Sweep: ending frequency in hertz.
    float end_hz = 0.0f;
    // Sweep: duration of one pass.
    float seconds = 0.0f;
    // Peak amplitude, 0..1. Values outside are clamped.
    float amplitude = 0.0f;
    // Sweep: whether to restart after reaching the end.
    bool repeat = false;

    static constexpr Signal silence() { return {}; }

    static constexpr Signal sine(float hz, float amplitude) {
        Signal s;
        s.kind = Kind::Sine;
        s.hz = hz;
        s.amplitude = amplitude;
        return s;
    }

    static constexpr Signal white_noise(float amplitude) {
        Signal s;
        s.kind = Kind::WhiteNoise;
        s.amplitude = amplitude;
        return s;
    }

    static constexpr Signal pink_noise(float amplitude) {
        Signal s;
        s.kind = Kind::PinkNoise;
        s.amplitude = amplitude;
        return s;
    }

    static constexpr Signal sweep(float start_hz, float end_hz, float seconds, float amplitude,
                                  bool repeat) {
        Signal s;
        s.kind = Kind::Sweep;
        s.start_hz = start_hz;
        s.end_hz = end_hz;
        s.seconds = seconds;
        s.amplitude = amplitude;
        s.repeat = repeat;
        return s;
    }

    friend constexpr bool operator==(const Signal&, const Signal&) = default;
};

// Human-readable name, for test failure messages and logs.
const char* to_string(Signal::Kind kind) noexcept;

// Generates a stimulus into a buffer.
//
// All state is preallocated; fill() never allocates. Owned by the thread that
// calls fill() - normally the audio callback. Changing the signal from another
// thread needs the caller's own hand-off.
class Generator {
public:
    // Build a generator. `sample_rate` must be positive.
    //
    // The seed makes noise reproducible, which matters for tests: a flaky
    // spectrum assertion is worse than no assertion.
    Generator(float sample_rate, Signal signal, std::uint64_t seed);

    // Change what is generated, resetting phase and filter state.
    void set_signal(Signal signal) noexcept;

    // The signal in force.
    Signal signal() const noexcept { return signal_; }

    // Return to the start.
    void reset() noexcept;

    // Whether a non-repeating sweep has run to its end.
    bool is_finished() const noexcept { return finished_; }

    // Fill `out` with the next samples, overwriting whatever was there.
    //
    // Real-time safe: no allocation, no locks, no branches on anything but the
    // signal kind.
    void fill(std::span<float> out) noexcept;

private:
    // A small xorshift generator.
    //
    // Noise does not need cryptographic quality, it needs to be fast, never
    // block, and be identical on every platform.
    struct Rng {
        explicit Rng(std::uint64_t seed) noexcept;

        // Uniform in [-1, 1).
        float next_sample() noexcept;

        std::uint64_t state;
    };

    // Paul Kellet's refined pink filter: white noise shaped to -3 dB per
    // octave, accurate to about +-0.05 dB from 9 Hz to 20 kHz. Six one-pole
    // sections plus a direct path, which is far cheaper than an FFT-based
    // approach and good enough that the error is invisible next to any real
    // acoustic measurement.
    struct PinkFilter {
        // Roughly normalises the summed output back to unity peak.
        static constexpr float kScale = 0.125f;

        float process(float white) noexcept;
        void reset() noexcept;

        float b[7] = {};
    };

    void fill_sine(std::span<float> out) noexcept;
    void fill_white(std::span<float> out) noexcept;
    void fill_pink(std::span<float> out) noexcept;
    void fill_sweep(std::span<float> out) noexcept;

    double sample_rate_;
    Signal signal_;
    // Radians, in double - see the file comment on drift.
    double phase_ = 0.0;
    // Samples into the current sweep pass.
    std::uint64_t position_ = 0;
    Rng rng_;
    PinkFilter pink_;
    bool finished_ = false;
};

}  // namespace analyzer::dsp
