// Real-to-complex FFT behind an interface.
//
// The interface exists for two reasons. It keeps a vDSP-backed implementation
// possible on Apple platforms without leaking Accelerate into the rest of the
// core, and it keeps the non-Apple ports open. Do not call an FFT library from
// anywhere but an implementation of Fft.

#pragma once

#include <cstddef>
#include <memory>
#include <span>

#include "dsp/complex.hpp"

namespace analyzer::dsp {

// A real-to-complex FFT of a fixed size, and its inverse.
//
// Real-time contract: forward() and inverse() must not allocate. The analysis
// thread runs them on every hop - several hundred times a second - and an
// allocation there is a latency hazard with an unbounded worst case.
// Implementations allocate everything they need at construction.
//
// Not thread-safe: one instance belongs to one thread at a time.
class Fft {
public:
    virtual ~Fft() = default;

    // Number of real samples per transform.
    virtual std::size_t size() const noexcept = 0;

    // Number of complex bins, size() / 2 + 1.
    //
    // The transform is one-sided: bin 0 is DC and bin size() / 2 is Nyquist,
    // both of which are purely real for real input.
    std::size_t bins() const noexcept { return size() / 2 + 1; }

    // Transform `input` (exactly size() samples) into `output` (exactly bins()
    // bins).
    //
    // Unnormalised: a full-scale DC input produces size() in bin 0. `input` is
    // not modified.
    //
    // A wrong buffer length is a programmer error and aborts rather than
    // returning a status the real-time path would have to check on every hop.
    virtual void forward(std::span<const float> input, std::span<Complex32> output) noexcept = 0;

    // Transform a one-sided spectrum (exactly bins() bins) back into size()
    // real samples.
    //
    // Also unnormalised, so forward() then inverse() scales by size(). The
    // imaginary parts of the DC and Nyquist bins are ignored: for a real
    // signal they are zero by definition.
    virtual void inverse(std::span<const Complex32> input, std::span<float> output) noexcept = 0;

protected:
    Fft() = default;
    Fft(const Fft&) = default;
    Fft& operator=(const Fft&) = default;
};

// Portable Fft over KissFFT.
//
// Any even size of at least 2 works. Powers of two are fastest, and are all the
// analysis uses.
class RealFft final : public Fft {
public:
    // Plan transforms of `size` real samples. `size` must be even and at least
    // 2: a one-sided real transform is not meaningful otherwise.
    explicit RealFft(std::size_t size);
    ~RealFft() override;

    RealFft(RealFft&&) noexcept;
    RealFft& operator=(RealFft&&) noexcept;
    RealFft(const RealFft&) = delete;
    RealFft& operator=(const RealFft&) = delete;

    std::size_t size() const noexcept override { return size_; }
    void forward(std::span<const float> input, std::span<Complex32> output) noexcept override;
    void inverse(std::span<const Complex32> input, std::span<float> output) noexcept override;

private:
    // KissFFT's plans, kept out of this header so nothing else can reach the
    // library directly. Defined in fft.cpp.
    struct Plans;

    std::size_t size_;
    std::unique_ptr<Plans> plans_;
};

}  // namespace analyzer::dsp
