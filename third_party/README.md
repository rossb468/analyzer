# Third-party code

Copied in rather than fetched, so a build needs no network and the exact code
compiled is in the history. Each is small, permissively licensed, and used
behind one wrapper so it can be swapped without touching the rest of the core.

| Library | Version | Licence | Used by | Why this one |
|---|---|---|---|---|
| [KissFFT](https://github.com/mborgerding/kissfft) | `e5e3fac` | BSD-3-Clause | `src/dsp/fft.cpp` only | Any even size, like the Rust `realfft` it replaces; no allocation per transform; plain C that can be read end to end. Apple's vDSP or pffft are faster and would slot in behind `dsp::Fft`. |
| [dr_wav](https://github.com/mackron/dr_libs) | `dfe8377` | Public domain or MIT-0 | `src/model/wav.cpp` only | One header, reads and writes every WAV variant the project meets. |
| [GoogleTest](https://github.com/google/googletest) | `v1.15.2`, `googletest/` only | BSD-3-Clause | `tests/` only | The industry-standard C++ test framework. Death tests check that broken preconditions abort with the right message. |
