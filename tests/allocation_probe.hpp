// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_TEST_ALLOCATION_PROBE_HPP
#define CEL_RUNTIME_TEST_ALLOCATION_PROBE_HPP
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>
#if defined(_WIN32)
#include <malloc.h>
#endif
/// Counts allocations only inside a bounded measurement.
inline bool count_allocations = false;
/// Counts both ordinary and aligned allocations.
inline std::size_t allocation_count = 0;
/// Fails exactly one selected allocation, then restores normal allocation.
inline int allocation_failure_after = -1;
/// Runs one explicitly installed acquisition hook, then clears it before invocation.
inline void (*allocation_hook)() = nullptr;
/// Records allocation and applies bounded one-shot failure injection.
inline void probe_allocation() {
    if (allocation_hook) {
        auto hook = allocation_hook;
        allocation_hook = nullptr;
        hook();
    }
    if (allocation_failure_after == 0) {
        allocation_failure_after = -1;
        throw std::bad_alloc();
    }
    if (allocation_failure_after > 0) --allocation_failure_after;
    if (count_allocations) ++allocation_count;
}
/// Allocates ordinary storage through the test instrumentation.
void* operator new(std::size_t size) {
    probe_allocation();
    if (auto* pointer = std::malloc(size == 0 ? 1 : size)) return pointer;
    throw std::bad_alloc();
}
/// Releases ordinary storage.
void operator delete(void* pointer) noexcept { std::free(pointer); }
/// Allocates aligned storage with the platform's matching allocation facility.
void* operator new(std::size_t size, std::align_val_t alignment) {
    probe_allocation();
    const auto align = static_cast<std::size_t>(alignment);
    const auto extent = size == 0 ? std::size_t{1} : size;
    if (extent > std::numeric_limits<std::size_t>::max() - (align - 1)) throw std::bad_alloc();
    const auto rounded = ((extent + align - 1) / align) * align;
#if defined(_WIN32)
    auto* pointer = _aligned_malloc(rounded, align);
#else
    auto* pointer = std::aligned_alloc(align, rounded);
#endif
    if (!pointer) throw std::bad_alloc();
    return pointer;
}
/// Releases aligned storage.
void operator delete(void* pointer, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}
/// Releases sized ordinary storage.
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
/// Releases sized aligned storage.
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}
/// Owns one bounded allocation measurement, restoring normal counting on exceptions.
struct allocation_probe {
    /// Starts a fresh measurement.
    allocation_probe() {
        allocation_count = 0;
        count_allocations = true;
    }
    /// Stops counting before test assertions allocate.
    ~allocation_probe() { count_allocations = false; }
    /// Disallows duplicated measurement ownership.
    allocation_probe(const allocation_probe&) = delete;
    /// Disallows duplicated assignment.
    allocation_probe& operator=(const allocation_probe&) = delete;
};
/// Owns bounded one-shot failure injection and disables it before recovery/assertions.
struct allocation_failure_probe {
    /// Fails the selected allocation after budget successful allocations.
    /// Precondition: budget is nonnegative.
    explicit allocation_failure_probe(int budget) {
        assert(budget >= 0);
        allocation_failure_after = budget;
    }
    /// Restores normal allocation on every exit.
    ~allocation_failure_probe() { allocation_failure_after = -1; }
    /// Disallows duplicated failure ownership.
    allocation_failure_probe(const allocation_failure_probe&) = delete;
    /// Disallows duplicated assignment.
    allocation_failure_probe& operator=(const allocation_failure_probe&) = delete;
};
#endif
