// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_INSTRUCTION_HPP
#define CEL_RUNTIME_INSTRUCTION_HPP
#include <cel/runtime/detail/raw_segment.hpp>
#include <memory_resource>
#include <vector>

namespace cel {
class compiled_segment;
} // namespace cel
namespace cel::detail {
class prepared_raw_segment;
/// A borrowed capture with its typed execution, destruction, and retirement operations.
///
/// The capture owner must retain the live object until destruction.
struct instruction {
    const void* capture;                   ///< Borrows its stable capture object.
    void (*execute)(const void*, frame&);  ///< Dispatches through the typed thunk.
    void (*destroy)(const void*) noexcept; ///< Destroys the actual capture type.
    void (*retire)(const void*, retirement_queue&) noexcept; ///< Detaches runtime ownership edges.
};
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
/// Checked preparation and trusted canonical capture publication.
struct segment_access {
    /// Binds canonical execution for internal callers; follows the checked preparation contract.
    ///
    /// Complexity: compiled_segment's checked signature binding costs.
    static prepared_raw_segment prepare_raw(const compiled_segment& program,
                                            std::span<const value_type> arguments,
                                            const value_type& result);
    /// Releases a mutable sequence into the supplied retirement operation.
    static void retire(raw_segment& segment, retirement_queue& queue) noexcept {
        segment._storage.retire(queue);
    }
    /// Releases frozen code into the supplied retirement operation without recursive destruction.
    static void retire(cel::compiled_segment& segment, retirement_queue& queue) noexcept;
    /// Releases a prepared binding's code, leaving its signature metadata owned by the binding.
    static void retire(cel::detail::prepared_raw_segment& segment,
                       retirement_queue& queue) noexcept;
    /// Stores a typed capture without requiring public value operands or fixed arity.
    ///
    /// F executes as const F&(frame&). Takes independent capture ownership; any referenced
    /// external objects retain their original ownership and lifetime obligations.
    ///
    /// \pre the callback enforces its complete ordered stack signature and cleans up partial
    /// result construction before propagating failure.
    ///
    /// Failure does not publish an operation or leave an unowned live capture.
    ///
    /// Complexity: callback construction and raw_segment append's amortized bookkeeping costs;
    /// owned capture storage.
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
