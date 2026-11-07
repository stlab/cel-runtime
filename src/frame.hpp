// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_FRAME_HPP
#define CEL_RUNTIME_FRAME_HPP
#include "storage.hpp"
#include <cel/runtime/raw_stack.hpp>
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
};
/// Supplies trusted canonical access for a later signature-checked instruction builder.
/// Operands never borrow mutable ownership; all pointers expire on stack mutation.
struct frame_access {
    /// Transfers one canonical result directly into external uninitialized bytes.
    /// Precondition: type matches the complete top descriptor and installed ownership pair;
    /// destination is aligned, disjoint, and large enough, and retains a live matching descriptor.
    /// Success retires the stack slot without destroying the transferred owner.
    /// Throws out_of_range on underflow and logic_error for an active nested frame.
    /// Complexity: O(outer canonical bytes) time; no allocation or shape comparisons.
    static void release_result(frame& scope, const value_type& type, std::byte* destination) {
        scope.require_active();
        scope._stack->release(scope._checkpoint, type, destination);
    }
    /// Borrows one matching initialized operand above the frame checkpoint.
    /// Precondition: complete type and ownership operations match the stored operand.
    /// Layout and depth checks remain effective in release builds.
    static const std::byte* operand(frame& scope, std::size_t from_top, const value_type& type) {
        scope.require_active();
        return scope._stack->borrow_trusted(scope._checkpoint, from_top, type);
    }
    /// Constructs one result using a writer responsible for partial-result cleanup.
    /// Precondition: writer initializes exactly type or releases every acquired resource.
    /// The source must not borrow stack bytes across potentially reallocating preparation.
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
    /// Complexity: O(count + visited payload ownership paths).
    static void discard(frame& scope, std::size_t count) {
        scope.require_active();
        scope.discard(count);
    }
};
} // namespace cel::detail
#endif
