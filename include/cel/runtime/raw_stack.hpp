// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_RAW_STACK_HPP
#define CEL_RUNTIME_RAW_STACK_HPP

#include <array>
#include <cel/runtime/value.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <tuple>
#include <utility>

namespace cel {
namespace detail {
class frame;
class raw_segment;
struct stack_state;
struct frame_access;
} // namespace detail

/// Owns reusable canonical operand bytes and complete live-slot metadata.
/// The owner outlives every borrowed frame. Moving rejects active frames.
/// Exclusive access is required, including during nested evaluation.
/// During canonical construction, operand acquisition, or retirement, reentrant mutation,
/// extraction, execution, and frame entry throw std::logic_error.
/// Ordinary operation callbacks may use the active frame or evaluate a nested frame.
/// Example: `raw_stack s; s.reserve(256, 256); s.push(2.0); auto n = s.pop<double>();`.
// [knowledge] Transfer only internal canonical carriers bytewise, never public facades.
// [knowledge] Keep the ownership-phase gate nonallocating and release it before ordinary callbacks.
class raw_stack {
    std::unique_ptr<detail::stack_state> _state;
    friend class detail::frame;
    friend struct detail::frame_access;

    /// Excludes reentrant stack access while stable canonical bytes or slots are required.
    /// Precondition: the state is idle; checked entry points establish this invariant.
    class phase_guard {
        detail::stack_state* _state;

    public:
        /// Acquires the stack-owned phase state without allocating.
        explicit phase_guard(raw_stack& stack) noexcept;
        /// Restores the idle state on normal and exceptional exit.
        ~phase_guard() noexcept;
        /// Disallows duplicated phase ownership.
        phase_guard(const phase_guard&) = delete;
        /// Disallows phase reassignment.
        phase_guard& operator=(const phase_guard&) = delete;
    };
    /// Creates the state of a moved-from reusable stack if needed.
    detail::stack_state& state();
    /// Rejects reentry during a canonical ownership phase.
    void require_idle() const;
    /// Rejects canonical-phase reentry and mutation outside the innermost active frame.
    void require_access(const detail::frame* owner) const;
    /// Materializes a checked top operand while keeping its source bytes stable.
    /// Throws out_of_range on underflow and invalid_argument on complete type mismatch.
    /// Complexity: O(payload + compared shape size) time and result space.
    template <class T>
    T read_value(std::size_t floor) {
        if (depth() <= floor) throw std::out_of_range("operand underflow");
        phase_guard phase(*this);
        const auto type = detail::concrete_adapter<T>::type();
        return detail::concrete_adapter<T>::read(type, borrow(floor, 0, type));
    }
    /// Prepares one aligned unowned slot without changing active depth or cursor.
    /// Complexity: O(retained bytes + slots) time and space on growth.
    std::size_t prepare(const value_type& type);
    /// Publishes one completely initialized payload after successful construction.
    void publish(const value_type& type, std::size_t offset) noexcept;
    /// Constructs canonical bytes with a writer that cleans partial ownership on failure.
    /// Precondition: writer initializes exactly type or retires all its partial ownership.
    /// Complexity: O(payload construction + retained bytes/slots on growth).
    void push_written(const value_type& type,
                      const void* source,
                      void (*write)(const void*, const value_type&, std::byte*));
    /// Copies one external owner directly through the canonical clone helper.
    /// Complexity: O(payload clone + retained bytes/slots on growth).
    void clone(const value& source);
    /// Transfers one external owner's canonical bytes, retiring its source.
    /// Complexity: O(outer bytes + retained bytes/slots on growth).
    void take(value& source);
    /// Borrows a checked slot above the protected floor.
    /// Throws out_of_range on underflow and invalid_argument on complete type mismatch.
    /// Complexity: O(compared shape size) time/space for distinct descriptor plans.
    const std::byte* borrow(std::size_t floor,
                            std::size_t from_top,
                            const value_type& expected) const;
    /// Borrows trusted canonical operands for a stored, signature-correct instruction.
    /// Precondition: expected is the complete matching operand type and operation pair.
    /// Checks byte extent/alignment and slot bounds before any access.
    const std::byte* borrow_trusted(std::size_t floor,
                                    std::size_t from_top,
                                    const value_type& expected) const;
    /// Retires a suffix without allocation or recursion with operand depth.
    /// Precondition: floor <= depth().
    /// Complexity: O(removed slots + visited payload ownership paths).
    void truncate(std::size_t floor) noexcept;
    /// Transfers the top slot to a public owner after acquiring outer storage.
    /// Throws out_of_range on underflow; allocation failure preserves the slot.
    /// Complexity: O(outer canonical bytes) time and space.
    value extract(std::size_t floor);
    /// Transfers top canonical bytes directly into uninitialized external result storage.
    /// Precondition: expected matches the top complete descriptor and ownership pair;
    /// destination is aligned, disjoint from stack storage, and large enough for expected.
    /// Throws out_of_range on underflow; checks the source layout before accessing bytes.
    /// Complexity: O(outer canonical bytes) time; no allocation or descriptor comparisons.
    void release(std::size_t floor, const value_type& expected, std::byte* destination);

public:
    /// Creates an empty reusable stack.
    raw_stack();
    /// Retires all live payloads and releases reserved storage.
    /// Precondition: no frame referring to this stack remains alive.
    /// Complexity: O(slots + visited payload ownership paths).
    ~raw_stack();
    /// Disallows duplicated mutable ownership.
    raw_stack(const raw_stack&) = delete;
    /// Disallows copied assignment.
    raw_stack& operator=(const raw_stack&) = delete;
    /// Transfers storage and live slots; leaves other empty and reusable.
    /// Throws logic_error if other has an active frame or canonical phase.
    raw_stack(raw_stack&& other);
    /// Releases prior payloads and transfers other; self-assignment is unchanged.
    /// Throws logic_error if either stack has an active frame or canonical phase.
    /// Complexity: O(old slots + visited old payload ownership paths).
    raw_stack& operator=(raw_stack&& other);
    /// Reserves bytes, alignment, and at least bytes live slots without changing the cursor.
    /// Zero-byte values may need reserve_depth in addition to byte reservation.
    /// Throws invalid_argument for invalid alignment and overflow_error for extent overflow.
    /// Throws logic_error if a frame is active.
    /// Complexity: O(retained initialized bytes + slots) time and allocated space on growth.
    /// Example: `stack.reserve(1024, 256);`.
    void reserve(std::size_t bytes, std::size_t alignment);
    /// Reserves bookkeeping for count simultaneously live operands, including zero-byte units.
    /// Throws logic_error if a frame is active.
    /// Complexity: O(live slots) time and O(count) space on growth.
    /// Example: `stack.reserve_depth(100);`.
    void reserve_depth(std::size_t count);
    /// Destroys every live payload while retaining reusable storage.
    /// Throws logic_error if a frame is active.
    /// Complexity: O(slots + visited payload ownership paths), without allocating.
    /// Example: `stack.clear();`.
    void clear();
    /// Returns the number of live operands, excluding spare initialized bytes.
    /// Example: `raw_stack{}.depth() == 0`.
    std::size_t depth() const noexcept;
    /// Returns the active byte cursor, including padding preceding live operands.
    /// Example: `raw_stack{}.cursor() == 0`.
    std::size_t cursor() const noexcept;
    /// Writes a concrete leaf or packed composite without constructing scalar value boxes.
    /// Throws logic_error if a frame is active; failure preserves live operands.
    /// Complexity: O(payload construction + retained bytes/slots on growth).
    /// Example: `stack.push(std::tuple{true, 2.0});`.
    template <class T>
    void push(const T& source) {
        require_access(nullptr);
        push_written(detail::concrete_adapter<T>::type(), &source,
                     [](const void* input, const value_type& type, std::byte* destination) {
                         detail::concrete_adapter<T>::write(*static_cast<const T*>(input), type,
                                                            destination);
                     });
    }
    /// Returns an independent concrete top value, retiring its canonical owner.
    /// Throws out_of_range on underflow, invalid_argument on complete type mismatch,
    /// and logic_error if a frame is active. Failure preserves the operand.
    /// Complexity: O(result payload size + compared shape size) time and result space.
    /// Example: `stack.push(true); auto flag = stack.pop<bool>();`.
    template <class T>
    T pop() {
        require_access(nullptr);
        auto result = read_value<T>(0);
        truncate(depth() - 1);
        return result;
    }
    /// Clones a public owner into canonical stack storage.
    /// Leaves source unchanged, preserving value's reference semantics for callable components.
    /// Throws logic_error if a frame is active; failure preserves all operands.
    /// Complexity: O(cloned payload + retained bytes/slots on growth) time and space.
    /// Example: `stack.push_clone(value::from(std::vector{2.0}));`.
    void push_clone(const value& source);
    /// Transfers canonical ownership, leaving source unit after success.
    /// Throws logic_error if a frame is active; failure preserves source and stack.
    /// Complexity: O(outer bytes + retained bytes/slots on growth).
    /// Example: `auto v = value::from(true); stack.push_take(v);`.
    void push_take(value& source);
    /// Removes the top operand and transfers its canonical ownership to the returned value.
    /// Throws out_of_range on underflow and logic_error if a frame is active.
    /// Failure preserves the stack.
    /// Complexity: O(outer bytes) time and space.
    /// Example: `auto result = stack.pop_value();`.
    value pop_value();
};

} // namespace cel
#endif
