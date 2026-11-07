// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_INSTRUCTION_HPP
#define CEL_RUNTIME_INSTRUCTION_HPP
#include <cel/runtime/raw_segment.hpp>
#include <memory_resource>
#include <vector>

namespace cel {
class compiled_segment;
class prepared_raw_segment;
} // namespace cel
namespace cel::detail {
/// Borrows one arena-owned constructed capture with typed dispatch and destruction.
struct instruction {
    const void* capture;                   ///< Borrows its stable capture object.
    void (*execute)(const void*, frame&);  ///< Dispatches through the typed thunk.
    void (*destroy)(const void*) noexcept; ///< Destroys the actual capture type.
    void (*retire)(const void*, retirement_queue&) noexcept; ///< Detaches runtime ownership edges.
};
/// Owns a standard stable arena and the destruction log for constructed captures.
struct instruction_storage : retirement_node {
    std::pmr::monotonic_buffer_resource arena; ///< Owns stable aligned object storage.
    std::vector<instruction> instructions;     ///< Owns dispatch/destruction records.
    /// Creates a stable arena whose last release detaches managed captures before deletion.
    instruction_storage() : retirement_node(dispose_storage) {}
    /// Retires managed edges, destroys real captures, and deletes their arena.
    /// Complexity: O(instructions + capture destruction work); no traversal allocation.
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
    /// Complexity: O(instructions + capture destruction work).
    ~instruction_storage() {
        // Captures have real object lifetimes; arena release alone cannot destroy them.
        for (auto it = instructions.rbegin(); it != instructions.rend(); ++it)
            it->destroy(it->capture);
    }
};
/// Bridges a later checked builder to arbitrary canonical instruction dispatch.
struct segment_access {
    /// Detaches a mutable operation sequence into the current last-owner worklist.
    static void retire(raw_segment& segment, retirement_queue& queue) noexcept {
        segment._storage.retire(queue);
    }
    /// Detaches a frozen executable handle without destroying descendant captures recursively.
    static void retire(cel::compiled_segment& segment, retirement_queue& queue) noexcept;
    /// Detaches a prepared executable handle while retaining its ordinary metadata cleanup.
    static void retire(cel::prepared_raw_segment& segment, retirement_queue& queue) noexcept;
    /// Stores a typed capture without requiring public value operands or fixed arity.
    /// F executes as const F&(frame&); it enforces its trusted complete stack signature.
    /// Its partial-result writers use frame_access and clean failures before publishing.
    /// Complexity: O(capture construction) time and stored space.
    template <class F>
    static void append(raw_segment& segment, F callback) {
        static_assert(std::is_nothrow_destructible_v<F>);
        auto* location = static_cast<F*>(segment.allocate_capture(sizeof(F), alignof(F)));
        auto* capture = std::construct_at(location, std::move(callback));
        try {
            segment.append(
                capture,
                [](const void* object, frame& scope) {
                    std::invoke(*static_cast<const F*>(object), scope);
                },
                [](const void* object) noexcept { std::destroy_at(static_cast<const F*>(object)); },
                [](const void* object, retirement_queue& queue) noexcept {
                    if constexpr (requires(F & capture) { capture.retire(queue); }) {
                        static_assert(noexcept(std::declval<F&>().retire(queue)));
                        const_cast<F*>(static_cast<const F*>(object))->retire(queue);
                    }
                });
        } catch (...) {
            std::destroy_at(capture);
            throw;
        }
    }
};
} // namespace cel::detail
#endif
