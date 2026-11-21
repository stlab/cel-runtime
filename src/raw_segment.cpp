// SPDX-License-Identifier: BSL-1.0
#include "instruction.hpp"
#include <cel/runtime/detail/raw_segment.hpp>
#include <memory_resource>
#include <utility>
#include <vector>

namespace cel::detail {
namespace {
/// A borrowed capture with its typed execution, destruction, and retirement operations.
///
/// The capture owner must retain the live object until destruction.
struct instruction {
    const void* capture;                   ///< Borrows its stable capture object.
    void (*execute)(const void*, frame&);  ///< Dispatches through the typed thunk.
    void (*destroy)(const void*) noexcept; ///< Destroys the actual capture type.
    void (*retire)(const void*, retirement_queue&) noexcept; ///< Detaches runtime ownership edges.
};
} // namespace
/// Stable capture storage with exactly-once destruction of constructed operations.
struct instruction_storage : retirement_node {
    std::pmr::monotonic_buffer_resource arena; ///< Owns stable aligned object storage.
    std::vector<instruction> instructions;     ///< Owns dispatch/destruction records.
    /// Creates empty capture storage with last-owner retirement.
    instruction_storage() : retirement_node(dispose_storage) {}
    /// Destroys stored captures and releases their storage, deferring managed descendant
    /// retirement.
    ///
    /// \pre base is the live instruction_storage selected by its retirement callback.
    ///
    /// Complexity: O(instruction count + capture retirement/destruction work) time without
    /// traversal allocation.
    static void dispose_storage(retirement_node& base, retirement_queue& queue) noexcept {
        auto& storage = static_cast<instruction_storage&>(base);
        for (auto it = storage.instructions.rbegin(); it != storage.instructions.rend(); ++it) {
            if (it->retire) it->retire(it->capture, queue);
            it->destroy(it->capture);
        }
        storage.instructions.clear();
        delete &storage;
    }
    /// Destroys captures before releasing their arena, in reverse construction order.
    ///
    /// Complexity: O(instructions + capture destruction work).
    ~instruction_storage() {
        // Captures have real object lifetimes; arena release alone cannot destroy them.
        for (auto it = instructions.rbegin(); it != instructions.rend(); ++it)
            it->destroy(it->capture);
    }
};
void segment_access::retire(raw_segment& segment, retirement_queue& queue) noexcept {
    segment._storage.retire(queue);
}
raw_segment::raw_segment() noexcept = default;
raw_segment::~raw_segment() = default;
raw_segment::raw_segment(raw_segment&& other) noexcept = default;
raw_segment& raw_segment::operator=(raw_segment&& other) noexcept {
    if (this != &other) _storage = std::move(other._storage);
    return *this;
}
void* raw_segment::allocate_capture(std::size_t size, std::size_t alignment) {
    if (!_storage)
        _storage =
            detail::retirement_ptr<detail::instruction_storage>(new detail::instruction_storage());
    return _storage->arena.allocate(size, alignment);
}
void raw_segment::append(const void* capture,
                         void (*execute)(const void*, frame&),
                         void (*destroy)(const void*) noexcept,
                         void (*retire)(const void*, detail::retirement_queue&) noexcept) {
    _storage->instructions.push_back({capture, execute, destroy, retire});
}
void raw_segment::execute(frame& scope) const {
    scope.require_active();
    try {
        if (!_storage) return;
        // Execute immutable dispatch records in stored order.
        for (const auto& instruction : _storage->instructions)
            instruction.execute(instruction.capture, scope);
    } catch (...) {
        scope.reset();
        throw;
    }
}
} // namespace cel::detail

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include <cel/runtime.hpp>
#include <doctest/doctest.h>

namespace {
/// Creates lazy executable ownership without evaluating nested branches.
/// Complexity: O(depth) time and owned space.
cel::compiled_segment lazy_chain(int depth) {
    cel::dyn_segment base;
    base.just(cel::value::from(true));
    for (int i = 0; i != depth; ++i) {
        cel::dyn_segment next;
        next.just(cel::value::from(true));
        next.logical_and(std::move(base));
        base = std::move(next);
    }
    return std::move(base).finish();
}
TEST_CASE("deep lazy code retained by a prepared binding retires on its last release") {
    auto program = lazy_chain(12000);
    auto prepared =
        cel::detail::segment_access::prepare_raw(program, {}, cel::value_type::leaf<bool>());
    cel::dyn_segment empty;
    auto replacement = std::move(empty).finish();
    auto replacement_binding =
        cel::detail::segment_access::prepare_raw(replacement, {}, cel::value_type{});
    program = std::move(replacement);
    CHECK_NOTHROW(prepared = std::move(replacement_binding));
}
} // namespace

#endif
