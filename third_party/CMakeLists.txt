# Vendored dependencies, each small enough to read in an afternoon and pinned by
# being copied in. See third_party/README.md for versions and licences.

# KissFFT: the real-to-complex FFT behind dsp/fft.hpp. Compiled as C, in single
# precision, without this project's warnings.
add_library(kissfft STATIC
    kissfft/kiss_fft.c
    kissfft/kiss_fftr.c)
target_include_directories(kissfft SYSTEM PUBLIC kissfft)
target_compile_definitions(kissfft PUBLIC kiss_fft_scalar=float)
# KissFFT takes a temporary buffer for sizes with prime factors above 5. With
# this it comes from the stack rather than malloc, so even an unusual transform
# size never allocates per call. MSVC has no <alloca.h>; there the heap path
# remains, and only matters for sizes this project does not use.
if(NOT MSVC)
    target_compile_definitions(kissfft PRIVATE KISS_FFT_USE_ALLOCA)
endif()
set_target_properties(kissfft PROPERTIES POSITION_INDEPENDENT_CODE ON)

# dr_wav: WAV reading and writing, single header.
add_library(dr_wav INTERFACE)
target_include_directories(dr_wav SYSTEM INTERFACE dr_wav)
