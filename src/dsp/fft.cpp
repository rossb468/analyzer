#include "dsp/fft.hpp"

#include <cstdlib>
#include <new>

#include "kiss_fftr.h"

#include "base/contract.hpp"

namespace analyzer::dsp {

namespace {

// KissFFT's complex type is a struct of two floats, real then imaginary - the
// layout std::complex<float> guarantees. The casts below rely on exactly that
// and on nothing else.
static_assert(sizeof(kiss_fft_cpx) == sizeof(Complex32));
static_assert(alignof(kiss_fft_cpx) <= alignof(Complex32));

kiss_fft_cpx* as_kiss(Complex32* bins) noexcept {
    return reinterpret_cast<kiss_fft_cpx*>(bins);
}

const kiss_fft_cpx* as_kiss(const Complex32* bins) noexcept {
    return reinterpret_cast<const kiss_fft_cpx*>(bins);
}

// Owns one KissFFT plan. A plan is one malloc'd block, released with free().
struct PlanDeleter {
    void operator()(kiss_fftr_state* plan) const noexcept { kiss_fftr_free(plan); }
};
using Plan = std::unique_ptr<kiss_fftr_state, PlanDeleter>;

Plan make_plan(std::size_t size, bool inverse) {
    Plan plan(kiss_fftr_alloc(static_cast<int>(size), inverse ? 1 : 0, nullptr, nullptr));
    if (!plan) {
        throw std::bad_alloc();
    }
    return plan;
}

}  // namespace

// KissFFT plans one direction at a time, so a two-way transform holds two.
struct RealFft::Plans {
    Plan forward;
    Plan inverse;
};

RealFft::RealFft(std::size_t size) : size_(size) {
    ANALYZER_EXPECTS(size >= 2 && size % 2 == 0, "FFT size must be even and at least 2");
    plans_ = std::make_unique<Plans>(Plans{make_plan(size, false), make_plan(size, true)});
}

RealFft::~RealFft() = default;
RealFft::RealFft(RealFft&&) noexcept = default;
RealFft& RealFft::operator=(RealFft&&) noexcept = default;

void RealFft::forward(std::span<const float> input, std::span<Complex32> output) noexcept {
    ANALYZER_EXPECTS(input.size() == size_, "input length must equal the planned FFT size");
    ANALYZER_EXPECTS(output.size() == bins(), "output length must equal size / 2 + 1");
    // Reads `input`, writes `output`, touches nothing else and allocates
    // nothing: the plan holds all its scratch.
    kiss_fftr(plans_->forward.get(), input.data(), as_kiss(output.data()));
}

void RealFft::inverse(std::span<const Complex32> input, std::span<float> output) noexcept {
    ANALYZER_EXPECTS(input.size() == bins(), "input length must equal size / 2 + 1");
    ANALYZER_EXPECTS(output.size() == size_, "output length must equal the planned FFT size");
    kiss_fftri(plans_->inverse.get(), as_kiss(input.data()), output.data());
}

}  // namespace analyzer::dsp
