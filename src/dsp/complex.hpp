// The complex type used for every spectrum in the core.
//
// std::complex<float> is guaranteed by the standard to be laid out as two
// adjacent floats, real then imaginary - the same layout as a C `float[2]` and
// as the bin type of every FFT library. That is what lets a spectrum be handed
// to KissFFT, vDSP or the C ABI without copying.

#pragma once

#include <complex>

namespace analyzer::dsp {

// One FFT bin, or one point of a frequency response.
using Complex32 = std::complex<float>;

}  // namespace analyzer::dsp
