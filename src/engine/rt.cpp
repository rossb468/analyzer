#include "engine/rt.hpp"

namespace analyzer::engine {

namespace detail {

thread_local int forbid_depth = 0;
thread_local int permit_depth = 0;

// Set by alloc_trap.cpp's static initialiser when that object is linked in.
// A weak link between the two: this file never names the trap, so the engine
// library does not pull it into executables that did not ask for it.
bool trap_linked = false;

}  // namespace detail

bool alloc_trap_installed() noexcept {
    return detail::trap_linked;
}

}  // namespace analyzer::engine
