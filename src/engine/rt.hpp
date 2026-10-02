// Mechanical enforcement of the no-allocation rule.
//
// Every real-time audio codebase has the rule that the callback must not
// allocate. Almost all of them enforce it by convention, code review and scar
// tissue, which means violations ship and surface as a click roughly once an
// hour on somebody else's machine. That bug class is close to undebuggable: it
// does not reproduce, and for a measurement tool it does not even announce
// itself - it silently corrupts the data.
//
// So it is enforced instead. rt_section() wraps the callback body in a guard
// that aborts the process on any allocation, and the same guard runs in CI.
//
// How it works
// ------------
// C++ lets a program replace the global operator new and operator delete. The
// target analyzer::alloc_trap does that (src/engine/alloc_trap.cpp): its
// operator new checks a thread-local "forbidden" depth and aborts with a
// message if it is non-zero. rt_section() raises that depth for the duration of
// the callable it runs, on the current thread only.
//
// Executables opt in by linking analyzer::alloc_trap - every test binary and
// debug tool here does. One that does not still works; the guard simply has
// nothing to report to, and rt_section() runs its callable directly. Release
// builds of the apps do not link it, so shipped code pays nothing.
//
// What it cannot see: allocations made directly with C's malloc, which is how
// C libraries allocate. The only C library on the audio path is KissFFT, and
// it is built to allocate its scratch on the stack (see third_party/).

#pragma once

#include <utility>

namespace analyzer::engine {

namespace detail {

// How many rt_section() scopes the current thread is inside, and how many
// permit_alloc() scopes inside those. Defined in rt.cpp; read by
// alloc_trap.cpp. Plain ints: each is only ever touched by its own thread.
extern thread_local int forbid_depth;
extern thread_local int permit_depth;

// Raises a depth for one scope. Restores it on the way out however the scope
// is left, so a callable that returns early cannot leave the thread trapped.
class DepthGuard {
public:
    explicit DepthGuard(int& depth) noexcept : depth_(depth) { ++depth_; }
    ~DepthGuard() { --depth_; }
    DepthGuard(const DepthGuard&) = delete;
    DepthGuard& operator=(const DepthGuard&) = delete;

private:
    int& depth_;
};

}  // namespace detail

// Run `f` with allocation forbidden on this thread, and return what it returns.
//
// Wrap the body of an audio callback in this.
template <class F>
decltype(auto) rt_section(F&& f) {
    const detail::DepthGuard guard(detail::forbid_depth);
    return std::forward<F>(f)();
}

// Permit allocation inside an rt_section().
//
// An escape hatch for the rare genuinely-safe case, and for diagnostics while
// tracking a violation down. Reaching for this in the audio path is a design
// smell: if something on that path needs to allocate, the allocation belongs
// somewhere else, not behind a waiver.
template <class F>
decltype(auto) permit_alloc(F&& f) {
    const detail::DepthGuard guard(detail::permit_depth);
    return std::forward<F>(f)();
}

// Whether this process linked analyzer::alloc_trap, i.e. whether rt_section()
// will actually catch anything.
bool alloc_trap_installed() noexcept;

}  // namespace analyzer::engine
