// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_INSTRUCTION_HPP
#define CEL_RUNTIME_INSTRUCTION_HPP
#include <cel/runtime/raw_segment.hpp>
#include <memory_resource>
#include <vector>

namespace cel::detail {
/// Borrows one arena-owned constructed capture with typed dispatch and destruction.
struct instruction {
    const void* capture;                   ///< Borrows its stable capture object.
    void (*execute)(const void*, frame&);  ///< Dispatches through the typed thunk.
    void (*destroy)(const void*) noexcept; ///< Destroys the actual capture type.
};
/// Owns a standard stable arena and the destruction log for constructed captures.
struct instruction_storage {
    std::pmr::monotonic_buffer_resource arena; ///< Owns stable aligned object storage.
    std::vector<instruction> instructions;     ///< Owns dispatch/destruction records.
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
                [](const void* object) noexcept {
                    std::destroy_at(static_cast<const F*>(object));
                });
        } catch (...) {
            std::destroy_at(capture);
            throw;
        }
    }
};
} // namespace cel::detail
#endif
