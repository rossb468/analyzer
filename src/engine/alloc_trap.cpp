// Replacement global operator new and operator delete that abort inside an
// rt_section(). See engine/rt.hpp for the why.
//
// Built as its own object library, analyzer::alloc_trap, and linked only into
// executables that want the check: tests and debug tools. A replacement
// operator new must be linked as an object, not pulled from a static library,
// or the linker may never look for it.
//
// Every replacement allocates with malloc and frees with free, which is what
// the default implementations do too. Only the check is added.

#include <cstdio>
#include <cstdlib>
#include <new>

#include "engine/rt.hpp"

namespace analyzer::engine::detail {

extern bool trap_linked;

namespace {

// Record that the trap is present before main() runs.
const bool registered = (trap_linked = true);

void check(std::size_t size) noexcept {
    if (forbid_depth > 0 && permit_depth == 0) {
        // No allocation here: a fixed message straight to stderr.
        std::fprintf(stderr,
                     "allocation of %zu bytes inside rt_section: the audio callback must not "
                     "allocate\n",
                     size);
        std::fflush(stderr);
        std::abort();
    }
}

void* allocate(std::size_t size) {
    check(size);
    if (void* memory = std::malloc(size == 0 ? 1 : size)) {
        return memory;
    }
    throw std::bad_alloc();
}

void* allocate_aligned(std::size_t size, std::align_val_t alignment) {
    check(size);
    const auto align = static_cast<std::size_t>(alignment);
#if defined(_MSC_VER)
    void* memory = _aligned_malloc(size == 0 ? 1 : size, align);
#else
    // aligned_alloc wants the size to be a multiple of the alignment.
    const std::size_t rounded = ((size == 0 ? 1 : size) + align - 1) / align * align;
    void* memory = std::aligned_alloc(align, rounded);
#endif
    if (memory) {
        return memory;
    }
    throw std::bad_alloc();
}

void release_aligned(void* memory) noexcept {
#if defined(_MSC_VER)
    _aligned_free(memory);
#else
    std::free(memory);
#endif
}

}  // namespace
}  // namespace analyzer::engine::detail

using analyzer::engine::detail::allocate;
using analyzer::engine::detail::allocate_aligned;
using analyzer::engine::detail::release_aligned;

// Freeing is never trapped. Releasing memory on the audio thread is just as
// bad as allocating it, but it only happens if something was allocated there
// first, which the trap already catches - and destructors of objects created
// elsewhere legitimately run in places that are hard to predict.

void* operator new(std::size_t size) {
    return allocate(size);
}
void* operator new[](std::size_t size) {
    return allocate(size);
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept {
    try {
        return allocate(size);
    } catch (...) {
        return nullptr;
    }
}
void* operator new(std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}
void* operator new[](std::size_t size, std::align_val_t alignment) {
    return allocate_aligned(size, alignment);
}

void operator delete(void* memory) noexcept {
    std::free(memory);
}
void operator delete[](void* memory) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
    std::free(memory);
}
void operator delete(void* memory, std::align_val_t) noexcept {
    release_aligned(memory);
}
void operator delete[](void* memory, std::align_val_t) noexcept {
    release_aligned(memory);
}
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept {
    release_aligned(memory);
}
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
    release_aligned(memory);
}
