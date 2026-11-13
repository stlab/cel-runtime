// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_FRAME_HPP
#define CEL_RUNTIME_FRAME_HPP
#include "storage.hpp"
#include <cel/runtime/detail/frame.hpp>
#include <vector>

namespace cel::detail {
/// A live canonical operand with retained destruction metadata.
struct stack_slot {
    value_type type;      ///< Retains the complete descriptor until payload retirement.
    std::size_t previous; ///< Restores the cursor, including preceding padding.
    std::size_t offset;   ///< Locates canonical bytes in reusable storage.
};
/// The exclusive state of a reusable operand stack and its evaluation scopes.
///
/// Zero-byte operands remain distinct live slots. Only the innermost frame mutates the
/// stack, and canonical phases exclude reentrant mutation, extraction, and frame entry.
struct stack_state {
    // [knowledge] slots describes exactly the initialized live owners. cursor excludes
    // spare initialized bytes; each slot.previous restores the prior byte cursor,
    // including padding. active borrows the innermost frame, and phase_active belongs
    // to the one current canonical ownership operation.
    storage bytes;                 ///< Owns bytewise-transferable canonical storage.
    std::vector<stack_slot> slots; ///< Owns exactly the initialized operand slots.
    std::size_t cursor = 0;        ///< Marks active bytes, not spare initialized storage.
    frame* active = nullptr;       ///< Borrows the innermost live frame.
    bool phase_active = false;     ///< Excludes reentry while canonical bytes/slots are unstable.
};
/// Trusted canonical access for signature-checked instructions.
///
/// Operand views acquire no ownership and expire on stack mutation or destruction.
/// Growth, payload, adapter, and cleanup complexity terms follow raw_stack.
struct frame_access {
    /// Borrows the caller-owned stack for nested execution after canonical phases end.
    ///
    /// Throws std::logic_error if a nested frame or canonical phase is active.
    static raw_stack& stack(frame& scope) {
        scope.require_active();
        return *scope._stack;
    }
    /// Reserves space for result after consuming the specified suffix.
    ///
    /// Does not consume operands. This internal reservation is permitted in the innermost
    /// frame; public stack reservation remains forbidden during frame lifetime.
    ///
    /// Throws std::logic_error if a nested frame or canonical phase is active,
    /// std::out_of_range for missing operands, and std::overflow_error for extent/depth overflow.
    /// Preserves every owner and cursor on failure; may retain increased capacity.
    ///
    /// Complexity: growth costs, including newly initialized spare bytes; no main-stack
    /// allocation when byte, alignment, and slot reservations suffice.
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
    /// An operand view borrowed for one canonical acquisition.
    ///
    /// The enclosing acquire call bounds every use; copies do not extend that lifetime.
    struct operand_view {
        frame& scope; ///< Borrows the frame for the enclosing acquisition call only.
        /// Borrows a matching operand above the frame checkpoint.
        ///
        /// \pre Complete type and ownership operations match the operand; the enclosing
        /// acquisition remains active throughout every use.
        /// The view aliases stack bytes, which remain stable until acquisition returns.
        ///
        /// Throws std::out_of_range on underflow and std::invalid_argument on layout mismatch.
        const std::byte* operand(std::size_t from_top, const value_type& type) const {
            return scope._stack->borrow_trusted(scope._checkpoint, from_top, type);
        }
    };
    /// Returns the reader's independent result while keeping canonical operands stable.
    ///
    /// \pre The reader retains neither operand_view nor stack-byte views beyond this call.
    ///
    /// Reentrant stack mutation/extraction, execution, and frame entry throw logic_error.
    /// Throws logic_error if a nested frame or another canonical phase is active.
    /// Reader failure preserves live operands and restores ordinary stack access; client
    /// effects are not rolled back.
    ///
    /// Complexity: reader time, result storage, and auxiliary space; exclusion itself
    /// takes constant time and allocates nothing.
    template <class F>
    static decltype(auto) acquire(frame& scope, F reader) {
        scope.require_active();
        raw_stack::phase_guard phase(*scope._stack);
        return reader(operand_view{scope});
    }
    /// Removes the top suffix operand and transfers its owner to destination.
    ///
    /// \pre type matches the complete top descriptor and installed ownership pair;
    /// destination is aligned, disjoint, and large enough, and retains a live matching descriptor.
    /// The transferred payload is not destroyed; the checkpoint prefix remains live.
    ///
    /// Throws std::out_of_range on underflow, std::invalid_argument on source layout
    /// mismatch, and std::logic_error for an active nested frame or canonical phase.
    /// Failure preserves the operand and destination.
    ///
    /// Complexity: O(1 + outer canonical bytes) time; no allocation or shape comparisons.
    static void release_result(frame& scope, const value_type& type, std::byte* destination) {
        scope.require_active();
        scope._stack->release(scope._checkpoint, type, destination);
    }
    /// Borrows one matching initialized operand above the frame checkpoint.
    ///
    /// \pre Complete type and ownership operations match the stored operand.
    /// Layout and depth checks remain effective in release builds.
    /// This immediate view expires on mutation; use acquire for allocating operand reads.
    ///
    /// Throws std::logic_error if a canonical phase or nested frame is active,
    /// std::out_of_range on underflow, and std::invalid_argument on layout mismatch.
    static const std::byte* operand(frame& scope, std::size_t from_top, const value_type& type) {
        scope.require_active();
        return scope._stack->borrow_trusted(scope._checkpoint, from_top, type);
    }
    /// Appends one canonical result constructed by write.
    ///
    /// \pre The writer initializes exactly type or releases every acquired resource.
    /// The source must not borrow stack bytes across potentially reallocating preparation.
    ///
    /// Throws std::logic_error if a canonical phase or nested frame is active.
    /// Failure preserves live operands and cursor; reservation may grow.
    ///
    /// Complexity: writer work plus growth time; constructed payload and growth storage,
    /// plus writer auxiliary space.
    static void push(frame& scope,
                     const value_type& type,
                     const void* source,
                     void (*write)(const void*, const value_type&, std::byte*)) {
        scope.require_active();
        scope._stack->push_written(type, source, write);
    }
    /// Retires consumed operands above the checkpoint.
    ///
    /// Preserves the checkpoint prefix and reusable storage.
    ///
    /// Throws std::out_of_range on underflow without changing the suffix, and
    /// std::logic_error if a canonical phase or nested frame is active.
    ///
    /// Complexity: O(count) time plus cleanup work; no traversal allocation.
    static void discard(frame& scope, std::size_t count) {
        scope.require_active();
        scope.discard(count);
    }
};
} // namespace cel::detail
#endif
