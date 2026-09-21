#pragma once

#include "configuration.h"
#include <cstddef>
#include <cstdlib>
#include <utility>

// PsramAllocator: a std::allocator replacement that takes its memory from PSRAM.
//
// For node-based containers whose individual allocations are too small to cross the
// heap_caps_malloc_extmem_enable() threshold set in main.cpp, and which would therefore all land in
// internal DRAM however much PSRAM is free. NodeDB's four per-NodeNum satellite maps are the case
// this exists for: a few hundred red-black-tree nodes of 50-120 bytes each, allocated and freed as
// nodes come and go, which costs internal RAM twice - once for the bytes, again for the
// fragmentation that churn leaves behind.
//
// Falls back to malloc() where there is no PSRAM, or where it is exhausted, so non-PSRAM boards and
// the native test build behave exactly as they did before.

namespace memory
{

template <class T> struct PsramAllocator {
    using value_type = T;

    PsramAllocator() noexcept = default;
    // Containers rebind the allocator to their private node type; this is how that copy is made.
    template <class U> PsramAllocator(const PsramAllocator<U> &) noexcept {}

    T *allocate(std::size_t n)
    {
        const std::size_t bytes = n * sizeof(T);
#if defined(ARCH_ESP32) && defined(BOARD_HAS_PSRAM)
        if (void *p = ps_malloc(bytes))
            return static_cast<T *>(p);
#endif
        return static_cast<T *>(malloc(bytes));
    }

    void deallocate(T *p, std::size_t) noexcept { free(p); } // ps_malloc is malloc-family
};

// Every instantiation draws on the same pool, so any two compare equal. Containers rely on this to
// free a rebound node through a different allocator object than the one that allocated it.
template <class T, class U> inline bool operator==(const PsramAllocator<T> &, const PsramAllocator<U> &) noexcept
{
    return true;
}

template <class T, class U> inline bool operator!=(const PsramAllocator<T> &, const PsramAllocator<U> &) noexcept
{
    return false;
}

} // namespace memory
