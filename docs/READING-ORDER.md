# Reading order

A guided tour of the code for a C++ developer who is new to this codebase and to
audio measurement. The files are in the order that lets each one lean on the
ones before it: first the small vocabulary every module uses, then the signal
processing, then the threading that carries audio to the screen, then the
platform and the edges.

Every file has a header comment that says what it is for and why. This tour
points at what each file *teaches* - the DSP idea and the C++ technique - and at
the test that shows it working. Read the header first, then the test, then the
implementation. The tests are written as specifications: each name is a
sentence, and each comment says which bug the test came from.

For the rules, see `CLAUDE.md`; for the reasoning, `docs/HANDOFF.md`; for the
house style, `docs/CPP-CONVENTIONS.md`. To build and run:
`cmake -S . -B build/dev && cmake --build build/dev -j8 && ctest --test-dir build/dev -j8`.

If you only have an evening, jump to "The files worth discussing in an
interview" at the end.

## A little audio measurement first

Four ideas are enough to read everything below.

- **A spectrum analyzer** takes a short frame of samples, tapers it with a
  *window*, runs an FFT and reports the level in each *bin*. Averaging many
  overlapped frames makes a noisy estimate stable.
- **dBFS** is level relative to a full-scale sine. A calibration offset turns it
  into absolute dB SPL.
- **A transfer function** is what a system did to a signal: measure what went in
  and what came out at the same time and divide. Coherence says how far to trust
  it.
- **The audio callback** is called by the hardware on a thread with a hard
  deadline (2.67 ms at 128 frames and 48 kHz). Missing it is an audible click and
  a corrupted measurement, so it may do almost nothing, and the rest of the
  system is shaped around that.

## 1. The vocabulary: `src/base/`

**`src/base/contract.hpp`** - the whole file is one function and one macro.
`ANALYZER_EXPECTS(cond, "message")` aborts with the message in *every* build,
where `assert` vanishes under `NDEBUG`. The C++ to notice: a macro is the right
tool here (it captures `__FILE__`, `__LINE__` and the stringised condition), the
failure path is `[[noreturn]]` and `noexcept` so it is safe on the audio thread,
and a broken precondition is deliberately not an exception. Read
`docs/CPP-CONVENTIONS.md` "Errors" alongside it for the three failure mechanisms.
The tests that pin it are **death tests**: `tests/dsp/fft_test.cpp` has
`RealFftDeathTest.WrongInputLengthAborts`, which uses `EXPECT_DEATH(expr,
"message")` to prove the process aborts with that text.

**`src/base/numeric.hpp`** - `saturating_cast<To>(float)`. Casting an out-of-range
float to an integer is undefined behaviour in C++, and DSP code turns computed
floats (bin indices, tick counts) into integers constantly. The technique:
`constexpr`, constrained with `requires`, with the bounds built as exact powers
of two because `max()` of a 32-bit integer is not representable as a float.
`tests/base/numeric_test.cpp` tries exactly the inputs a bare cast gets wrong.

`src/base/number_text.hpp` is the third piece: float formatting and parsing that
is locale-independent and byte-stable, built on `std::to_chars`. Skim it, and
read its header for why `printf` would not do.

## 2. The signal path: `src/dsp/`

**`src/dsp/window.hpp`, `window.cpp`** - why a frame must be tapered before an
FFT, and the two correction factors (coherent gain for sinusoid level, equivalent
noise bandwidth for noise level) that every reading depends on. The C++: a
small value type, `WindowKind`, with named constructors
(`WindowKind::tukey(0.25f)`) instead of a bare enum, because one shape carries a
parameter. `Window` precomputes at construction so applying it never allocates.
Test: `tests/dsp/window_test.cpp` checks the factors against published values.

**`src/dsp/fft.hpp`, `fft.cpp`** - the one place the FFT library is touched.
`Fft` is an abstract interface (so vDSP could slot in on Apple platforms);
`RealFft` implements it over KissFFT. Look at how little of KissFFT leaks:
`struct Plans` is declared in the header and defined in the `.cpp`, so nothing
else can include the library (the **pimpl idiom**, with `std::unique_ptr` and an
out-of-line destructor, which is why the destructor and move operations are
defaulted in the `.cpp`). Also: spans for buffers, `static_assert` on the
layout equivalence between `kiss_fft_cpx` and `std::complex<float>`, and
`noexcept` on `forward()` because it is called on every hop. Tests:
`tests/dsp/fft_test.cpp` - Parseval's theorem as a correctness check, and the
death tests.

**`src/dsp/spectrum.hpp`, `spectrum.cpp`** - Welch's method: overlapped frames,
windowed, FFT'd, power-averaged. The level convention (0 dBFS = full-scale sine)
is the thing every number in the product depends on; the header derives it.
The C++: a streaming analyzer that accepts samples in arbitrary chunk sizes and
produces the same answer as one big block
(`SpectrumAnalyzer.StreamingInChunksMatchesOneBlock`). Test the known-answer
properties first: `FullScaleSineReadsZeroDbFs`, `MeasuredLevelIsIndependentOfWindow`.

**`src/dsp/transfer.hpp`, `transfer.cpp`** - the H1 estimator: average the
auto- and cross-spectra, *then* divide. Averaging the ratio is the classic
mistake, and the header explains why. Coherence needs more than one frame to
mean anything. This is where the dsp module's central idea lives; the engine
below just feeds it.

After that, in whatever order interests you: `dsp/biquad.*` (RBJ filters,
including `prewarp()`), `dsp/eq.*` (graphic and parametric EQ on one
implementation), `dsp/optimise.*` (greedy PEQ fit; read why boost is capped far
below cut), `dsp/deconv.*` (regularised deconvolution of a sweep into an impulse
response), `dsp/ir.*` (gating, Schroeder decay, EDT/T20/T30), `dsp/delay.*`
(GCC-PHAT), `dsp/mtw.*`, `dsp/meter.*`, `dsp/octave.*`, `dsp/generator.*`.

## 3. Getting audio across threads: `src/engine/`

This is the part most worth reading slowly. Three small files, each a lock-free
primitive or a guard, then the thing that composes them.

**`src/engine/ring.hpp`, `ring.cpp`** - a single-producer single-consumer ring of
interleaved samples. Two monotonically increasing counters, one written by each
thread. The producer does a **release** store of `written` after copying the
samples; the consumer does an **acquire** load before reading them; the same
pair runs the other way for `read`, so the producer never overwrites a slot the
consumer has not finished with. No lock and no compare-and-swap, because each
counter has exactly one writer. Things to notice: relaxed loads of your *own*
counter (only you write it), `alignas(64)` to keep the counters on separate cache
lines (**false sharing**), counters that grow forever and are reduced modulo the
capacity only at the copy, **all-or-nothing writes** with an overrun counter so a
dropped block is counted and never half-written, and **move-only endpoints**
(`CaptureSink`, `CaptureSource`) that make "exactly one producer" a compile-time
fact. `tests/engine/ring_test.cpp` has the single-thread semantics and a
cross-thread test (`CrossingThreadsPreservesOrder`); run it under ThreadSanitizer
(`-DANALYZER_SANITIZE=thread`). Weakening the release store to relaxed makes it
fail there, which is the point of that test.

**`src/engine/snapshot.hpp`** - a **triple buffer** for publishing finished
results to the UI. Three copies of the data; the analysis thread writes into one
nobody is reading and swaps it with the "back" slot in a **single atomic
exchange** (`acq_rel`); the UI does one exchange to take the newest. Neither side
ever waits, and the UI never sees a half-written frame. The state is one
`std::atomic<std::uint8_t>`: two bits of buffer index and a "fresh" flag. It is
header-only because it is a template, `publish_with(fill)` mutates in place so
publishing never allocates, and the slots are `alignas(64)`. Read the "How it
works" comment, then `tests/engine/snapshot_test.cpp`
(`ASlowReaderSeesOnlyTheNewestValue` is the rate-mismatch idea in one test).

**`src/engine/rt.hpp`, `alloc_trap.cpp`** - how "the audio callback must not
allocate" is *enforced*. The C++ language lets a program replace the global
`operator new`; `alloc_trap.cpp` does, and its replacement aborts if a
thread-local depth counter says the thread is inside an `rt_section()`.
`rt_section(f)` is a template that raises the counter with a small RAII guard
(`DepthGuard`, restored however the scope exits) and runs `f`. Things to notice:
the trap is an `OBJECT` library so the linker actually takes it; only test
binaries and the Debug CLI link it, so Release pays nothing; it cannot see
`malloc` directly; and `permit_alloc` exists but is called a design smell in
its own comment. The test is a death test:
`AllocTrapDeathTest.AllocatingInsideAnRtSectionAbortsTheProcess` in
`tests/engine/rt_test.cpp`.

**`src/engine/engine.hpp`, `engine.cpp`** - composes the three: `Engine::start()`
returns the `CaptureSink` for the audio callback and spawns a worker that drains
the ring, runs the analyzers and publishes `SpectrumFrame`s. Read the diagram at
the top. Then find `kDrainFrames` in the `.cpp`: a bounded read per iteration,
because draining until empty lets a fast producer starve publication - the
comment and `Engine.AFastProducerCannotStarvePublication` tell the story. Also
notice that the worker preallocates everything (the snapshot's vectors are sized
at creation), that stopping joins the thread in the destructor, and that the one
mutex in the file guards the measurement recording buffer on the *analysis*
side, never touching the audio callback. `engine/delay_line.hpp` is the small
helper for delay compensation in transfer mode.

## 4. Audio devices: `src/audio/`

**`src/audio/stream.hpp`** - the contract between hardware and code.
`AudioBuffers` is a non-owning view (spans) of one callback's interleaved
input and output; `ChannelView` is a strided iterator over one channel;
`AudioCallback` documents the real-time contract in prose and is `noexcept`
because an exception cannot unwind through a platform's C frames. Notice
`AudioBuffers`' constructor aborting on a length mismatch rather than
"correcting" it.

**`src/audio/backend.hpp`, `src/audio/offline.hpp`, `offline.cpp`** - an abstract
`AudioBackend` and its first implementation, which reads from memory instead of
hardware. The offline backend is why the whole analysis chain is testable with
no sound card and why the CLI can analyse a WAV file: a good example of an
interface shaped by a real second implementation. `tests/audio/offline_test.cpp`
exercises it, and `tests/audio/stream_test.cpp` runs callbacks inside `rt_section`.

**`src/audio/platform.hpp`, `target.hpp`** - how platform selection stays in one
place: `target.hpp` turns compiler predefines into two always-defined macros,
`default_backend()` picks the backend, and on a platform with none you get an
`UnavailableBackend` that enumerates nothing and refuses to open, so everything
above still builds and runs.

**`src/audio/coreaudio.hpp`, `coreaudio.cpp`** - the macOS backend, written
against the HAL directly. The header names no Apple type (so `platform.cpp`
compiles without the SDK); every Apple type is confined to the `.cpp`, which
CMake builds only on macOS. Read the IOProc section: scratch buffers sized once
at open, a `noexcept` callback, and the property helpers that `memcpy` out of
the HAL's byte blobs instead of casting. `ios.hpp` and `ios.mm` are the iOS
counterpart in Objective-C++, built with ARC. Neither can run in CI on Linux;
that is a deliberate limit of what the tests can claim.

## 5. The C boundary: `include/analyzer.h` and `src/ffi/`

**`include/analyzer.h`** - the contract with the Swift apps: plain C, POD structs
and opaque handles, no ownership crossing the boundary except through explicit
`_destroy` and `_stop` calls. Hand-maintained, so read its header comment for the
rules about changing it.

**`src/ffi/CMakeLists.txt`** - worth reading as C++ build engineering: the header
is C, but the implementation wants C++ declarations, so CMake generates a C++
view of it at configure time (taking the C23 enum branch and adding `noexcept` to
every declaration). A signature that drifts from the header becomes a compile
error. `src/ffi/internal.hpp` has `guard`, the template that makes every entry
point exception-safe: `extern "C"` plus `noexcept` plus a `try`/`catch(...)` that
returns a fallback, so nothing unwinds into Swift. `src/ffi/axis.cpp` is a short, typical
entry-point file; `src/ffi/session.hpp` shows who owns what and on which thread. `tests/ffi/c_compile_test.c` compiles the header as C11 and C23 and links
every entry point from C; the other tests in `tests/ffi/` call the entry points
as Swift would.

## 6. What gets stored and shown: `src/model/`, `src/cal/`, `src/plot/`

**`src/model/measurement.hpp`, `format.hpp`** - what a stored measurement is
(unsmoothed, complex, double precision, with absolute references and an
`std::optional` SPL offset) and the `.anlz` container: a text header followed by
raw little-endian doubles. Read the header comments for why, then
`tests/model/golden_test.cpp` for how byte-exact fixtures keep the format honest.
`model/error.hpp` is the exception hierarchy for setup failures.
`model/export.*` and `model/filter_export.*` write REW, Equalizer APO and miniDSP
text (the miniDSP exporter negates the feedback coefficients; the comment says
why).

**`src/cal/chain.hpp`** - the calibration chain, dBFS to dB SPL, and why it is
one module and not scattered.

**`src/plot/axis.hpp`, `reduce.hpp`** and `src/plot/README.md` - axis mapping that
the clients must call and never reimplement, and the reduction of linear FFT bins
onto a log axis. The module emits no pixels.

## 7. The edges: `src/cli/`

**`src/cli/run.cpp`, `main.cpp`** - `main()` is four lines; everything else is a
library (`analyzer_cli_lib`) so it can be unit-tested, which `tests/cli/` does by
calling `run(args, out, err)` with string streams. The CLI is also the
integration test: `--measure-demo` builds a synthetic room and measures it,
printing the constructed truth beside the answer. `tests/cli/golden_test.cpp`
replays a manifest of recorded commands (`tests/golden/README.md` explains the
comparison classes), and `src/cli/bench.cpp` is the benchmark behind `--bench`.

## The files worth discussing in an interview

These six carry the ideas a C++ audio DSP interviewer is most likely to probe.
Each can be explained from the page in a few minutes, and each has a test that
backs the claim.

1. **`src/engine/ring.cpp` (+ `ring.hpp`)** - memory ordering on real code.
   Why release on the counter you publish and acquire on the one you read, why
   relaxed is correct for your own counter, what false sharing is and where the
   `alignas(64)` is, why a failed write must leave nothing behind, and how a
   ThreadSanitizer run catches a wrong order. You can answer "write an SPSC
   queue" from this.
2. **`src/engine/snapshot.hpp`** - a wait-free hand-off with one atomic exchange,
   and the reasoning about roles rather than positions of three buffers. Good for
   "how does the UI read analysis results without blocking either thread?"
3. **`src/engine/rt.hpp` and `alloc_trap.cpp`** - turning a real-time rule into a
   check that fails a test. Replacement `operator new`, thread-local state,
   RAII, linker behaviour with static libraries, and the honest limit (it cannot
   see `malloc`). Strong for "how do you stop allocations on the audio thread?"
4. **`src/dsp/fft.hpp` and `fft.cpp`** - interface design and dependency
   hygiene: an abstract class for the seam, a pimpl that keeps a C library out of
   every other translation unit, a `noexcept` real-time contract, and a
   `static_assert` guarding a layout assumption. Good for "how would you swap in
   vDSP?"
5. **`src/dsp/spectrum.cpp` with `window.hpp`** - the domain core: window
   correction factors, the 0 dBFS convention, Welch averaging and streaming in
   arbitrary chunk sizes. Shows you can connect DSP theory to code, and the
   known-answer tests (a half-scale sine must read -6.02 dB) show how to verify
   it.
6. **`src/engine/engine.cpp`** - composition and a real bug: the
   `kDrainFrames` bound, the thread lifecycle, preallocation, and the one place a
   mutex is allowed and why. Good for "tell me about a concurrency bug you hit
   and how a test pins it."

Honourable mentions: `src/base/contract.hpp` (the argument for aborting rather
than throwing on a broken precondition, and death tests), `src/ffi/internal.hpp`
(exception safety at a C ABI), and `cmake/AnalyzerModule.cmake` (warnings as
errors, sanitizers, and `-ffp-contract=off` explained).
