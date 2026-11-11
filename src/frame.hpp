// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_FRAME_HPP
#define CEL_RUNTIME_FRAME_HPP
#include "storage.hpp"
#include <cel/runtime/detail/frame.hpp>
#include <vector>

namespace cel::detail {
/// Tracks one fully initialized canonical operand, retaining destruction metadata.
struct stack_slot {
    value_type type;      ///< Retains the complete descriptor until payload retirement.
    std::size_t previous; ///< Restores the cursor, including preceding padding.
    std::size_t offset;   ///< Locates canonical bytes in reusable storage.
};
/// Owns the relationship between byte storage, active slots, and lexical frames.
struct stack_state {
    storage bytes;                 ///< Owns bytewise-transferable canonical storage.
    std::vector<stack_slot> slots; ///< Owns exactly the initialized operand slots.
    std::size_t cursor = 0;        ///< Marks active bytes, not spare initialized storage.
    frame* active = nullptr;       ///< Borrows the innermost live frame.
    bool phase_active = false;     ///< Excludes reentry while canonical bytes/slots are unstable.
};
/// Supplies trusted canonical access for a later signature-checked instruction builder.
/// Operands never borrow mutable ownership; all pointers expire on stack mutation.
struct frame_access {
    /// Borrows the caller-owned stack for nested execution after canonical phases end.
    static raw_stack& stack(frame& scope) {
        scope.require_active();
        return *scope._stack;
    }
    /// Allocates final stack extent/slot capacity before transferring a consumed suffix.
    /// Throws out_of_range for missing operands and overflow_error for extent/depth overflow.
    /// Preserves every owner and cursor on failure; may retain increased capacity.
    /// Complexity: O(retained bytes/slots) on growth; no allocation on reserved storage.
    static void reserve_result(frame& scope, const value_type& result, std::size_t consumed) {
        scope.require_active();
        auto& data = scope._stack->state();
        if (consumed > data.slots.size() - scope._checkpoint)
            throw std::out_of_range("transfer operand underflow");
        raw_stack::phase_guard phase(*scope._stack);
        const auto remaining = data.slots.size() - consumed;
        const auto cursor = consumed == 0 ? data.cursor : data.slots[remaining].previous;
        const auto bytes = result.byte_layout();
        const auto extent = checked_add(align_offset(cursor, bytes.alignment), bytes.size);
        data.slots.reserve(checked_add(remaining, 1));
        data.bytes.reserve(extent, bytes.alignment);
        if (extent > data.bytes.size()) data.bytes.resize(extent);
    }
    /// Borrows operands only while a scoped canonical acquisition holds the phase gate.
    struct operand_view {
        frame& scope; ///< Borrows the frame for the enclosing acquisition call only.
        /// Borrows a matching operand for materialization before the acquisition returns.
        /// Precondition: complete type and ownership operations match the operand.
        const std::byte* operand(std::size_t from_top, const value_type& type) const {
            return scope._stack->borrow_trusted(scope._checkpoint, from_top, type);
        }
    };
    /// Materializes an independent result while keeping canonical operand bytes stable.
    /// Precondition: the reader does not retain stack-byte views beyond this call.
    /// Reentrant stack mutation/extraction, execution, and frame entry throw logic_error.
    /// Throws logic_error if a nested frame or another canonical phase is active.
    /// Complexity: O(reader work) time and result space; the gate does not allocate.
    template <class F>
    static decltype(auto) acquire(frame& scope, F reader) {
        scope.require_active();
        raw_stack::phase_guard phase(*scope._stack);
        return reader(operand_view{scope});
    }
    /// Transfers one canonical result directly into external uninitialized bytes.
    /// Precondition: type matches the complete top descriptor and installed ownership pair;
    /// destination is aligned, disjoint, and large enough, and retains a live matching descriptor.
    /// Success retires the stack slot without destroying the transferred owner.
    /// Throws out_of_range on underflow and logic_error for an active nested frame or phase.
    /// Complexity: O(outer canonical bytes) time; no allocation or shape comparisons.
    static void release_result(frame& scope, const value_type& type, std::byte* destination) {
        scope.require_active();
        scope._stack->release(scope._checkpoint, type, destination);
    }
    /// Borrows one matching initialized operand above the frame checkpoint.
    /// Precondition: complete type and ownership operations match the stored operand.
    /// Layout and depth checks remain effective in release builds.
    /// This immediate view expires on mutation; use acquire for allocating operand reads.
    /// Throws logic_error if a canonical phase or nested frame is active.
    static const std::byte* operand(frame& scope, std::size_t from_top, const value_type& type) {
        scope.require_active();
        return scope._stack->borrow_trusted(scope._checkpoint, from_top, type);
    }
    /// Constructs one result using a writer responsible for partial-result cleanup.
    /// Precondition: writer initializes exactly type or releases every acquired resource.
    /// The source must not borrow stack bytes across potentially reallocating preparation.
    /// Throws logic_error if a canonical phase or nested frame is active.
    /// Complexity: O(result construction + retained bytes/slots on growth).
    static void push(frame& scope,
                     const value_type& type,
                     const void* source,
                     void (*write)(const void*, const value_type&, std::byte*)) {
        scope.require_active();
        scope._stack->push_written(type, source, write);
    }
    /// Retires consumed operands above the checkpoint.
    /// Throws out_of_range on underflow.
    /// Throws logic_error if a canonical phase or nested frame is active.
    /// Complexity: O(count + visited payload ownership paths).
    static void discard(frame& scope, std::size_t count) {
        scope.require_active();
        scope.discard(count);
    }
};
} // namespace cel::detail
#endif
