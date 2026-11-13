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
/// \addtogroup runtime_stack
/// @{

namespace detail {
class frame;
class raw_segment;
struct stack_state;
struct frame_access;
} // namespace detail

/// An exclusive owner of reusable canonical operand storage.
///
/// Every live operand retains its complete type and ownership metadata. Zero-byte values
/// occupy logical slots. The stack outlives every borrowed frame.
/// One evaluator has exclusive access, including during nested evaluation; this is not
/// a synchronization primitive.
/// During canonical construction, operand acquisition, or retirement, reentrant mutation,
/// extraction, execution, and frame entry throw std::logic_error.
/// Ordinary operation callbacks may use the active frame or evaluate a nested frame.
///
/// Growth costs O(B + L + Z) time and O(Cb + Cs) reserved storage: B is the retained
/// initialized byte extent, L is the live slot count, Z is the newly initialized byte
/// extent, and Cb and Cs are the resulting byte and slot capacities. These costs apply
/// only when the corresponding storage grows or gains initialized bytes.
/// Payload work includes logical zero-byte elements, ownership-path lookup, and client
/// operations. Native adapter costs include first-use descriptor construction and
/// nesting-dependent auxiliary space. Cleanup work includes retired ownership paths,
/// last-owned executable nodes, edges, and instructions, released metadata, and client
/// destructors; runtime retirement traversal allocates nothing and uses constant stack space.
///
/// Example: `raw_stack s; s.reserve(256, 256); s.push(2.0); auto n = s.pop<double>();`.
// [knowledge] Transfer only internal canonical carriers bytewise, never public facades.
// [knowledge] Keep the ownership-phase gate nonallocating and release it before ordinary callbacks.
class raw_stack {
    // [knowledge] A moved-from stack has no state; state() restores it on demand.
    /// Owns live operands, reservation, and frame/phase exclusion state.
    std::unique_ptr<detail::stack_state> _state;
    friend class detail::frame;
    friend struct detail::frame_access;

    /// An allocation-free exclusion scope for canonical ownership operations.
    ///
    /// \pre The stack is idle. Checked entry points establish this obligation.
    class phase_guard {
        /// Borrows the stack's exclusion state until scope exit.
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
    /// Returns the owned state, restoring an empty moved-from stack if needed.
    ///
    /// Allocation failure leaves a moved-from stack empty and reusable.
    detail::stack_state& state();
    /// Checks that no canonical ownership phase is active.
    ///
    /// Throws std::logic_error on phase reentry.
    void require_idle() const;
    /// Checks mutation access for owner, or unframed access when owner is null.
    ///
    /// Throws std::logic_error during a canonical phase or when owner is not the
    /// innermost active frame.
    void require_access(const detail::frame* owner) const;
    /// Returns an independent concrete copy of the top operand above floor.
    ///
    /// \pre Access is authorized and no canonical phase is active.
    /// Source bytes stay stable during materialization; success does not consume the slot.
    ///
    /// Throws std::out_of_range on underflow and std::invalid_argument on complete type
    /// mismatch. Failure preserves the operand.
    ///
    /// Complexity: native read work plus expected O(P + E + S) comparison time;
    /// native result storage and adapter auxiliary space plus O(P + E) comparison
    /// auxiliary space. P, E, and S count visited descriptor pairs, edges, and compared
    /// name bytes; shared plans require no traversal.
    template <class T>
    T read_value(std::size_t floor) {
        if (depth() <= floor) throw std::out_of_range("operand underflow");
        phase_guard phase(*this);
        const auto type = detail::concrete_adapter<T>::type();
        return detail::concrete_adapter<T>::read(type, borrow(floor, 0, type));
    }
    /// Reserves an aligned uninitialized operand extent and returns its byte offset.
    ///
    /// \pre The caller holds a canonical phase.
    /// Live depth, cursor, and owners remain unchanged.
    ///
    /// Throws std::overflow_error for byte extent or logical depth overflow.
    /// Failure preserves live operands and cursor; increased capacity or initialized
    /// spare bytes may remain.
    ///
    /// Complexity: growth costs.
    std::size_t prepare(const value_type& type);
    /// Adds one initialized canonical owner to the live stack.
    ///
    /// \pre prepare(type) supplied offset and reserved a slot; that extent now contains
    /// exactly one initialized owner of type. The caller holds a canonical phase.
    void publish(const value_type& type, std::size_t offset) noexcept;
    /// Appends one canonical owner constructed by write.
    ///
    /// \pre Access is authorized and no canonical phase is active. The writer initializes
    /// exactly type or retires all partial ownership on failure. Source and type remain
    /// valid across preparation; source does not borrow potentially relocated stack bytes.
    ///
    /// Failure preserves live operands and cursor; reservation may grow.
    ///
    /// Complexity: writer work plus growth time; constructed payload and growth storage,
    /// plus writer auxiliary space.
    void push_written(const value_type& type,
                      const void* source,
                      void (*write)(const void*, const value_type&, std::byte*));
    /// Appends a clone of source with independent mutable aggregate payloads.
    ///
    /// \pre Access is authorized and no canonical phase is active.
    /// Callable components retain reference semantics.
    ///
    /// Failure preserves source and live operands; reservation may grow.
    ///
    /// Complexity: payload clone work plus growth time; cloned payload and growth storage.
    void clone(const value& source);
    /// Appends source's canonical owner and leaves source unit.
    ///
    /// \pre Access is authorized and no canonical phase is active.
    ///
    /// Failure preserves source and live operands; reservation may grow.
    ///
    /// Complexity: O(1 + outer canonical bytes) time plus growth costs.
    void take(value& source);
    /// Borrows a complete-type-checked operand above floor.
    ///
    /// The view aliases stack storage and expires on mutation or destruction.
    ///
    /// Throws std::out_of_range on underflow and std::invalid_argument on complete type
    /// or layout mismatch.
    ///
    /// Complexity: expected O(P + E + S) time and O(P + E) auxiliary space for visited
    /// descriptor pairs P, edges E, and compared name bytes S; shared plans skip traversal.
    const std::byte* borrow(std::size_t floor,
                            std::size_t from_top,
                            const value_type& expected) const;
    /// Borrows one layout-checked canonical operand above floor.
    ///
    /// \pre expected matches the operand's complete type and ownership operation pair.
    /// The view aliases stack storage and expires on mutation or destruction.
    ///
    /// Throws std::out_of_range on underflow and std::invalid_argument on byte extent
    /// or alignment mismatch, before accessing operand bytes.
    const std::byte* borrow_trusted(std::size_t floor,
                                    std::size_t from_top,
                                    const value_type& expected) const;
    /// Retires operands above floor and restores the preceding byte cursor.
    ///
    /// \pre floor <= depth(); no canonical phase is active.
    /// Preserves the live prefix and reusable storage without allocation or
    /// nesting-dependent runtime cleanup stack.
    ///
    /// Complexity: O(removed slots) time plus cleanup work.
    void truncate(std::size_t floor) noexcept;
    /// Removes the top operand above floor and transfers it to a public owner.
    ///
    /// \pre Access is authorized and no canonical phase is active.
    ///
    /// Throws std::out_of_range on underflow; allocation failure preserves the slot.
    ///
    /// Complexity: O(1 + outer canonical bytes) time and result storage.
    value extract(std::size_t floor);
    /// Removes the top operand above floor and transfers its owner to destination.
    ///
    /// \pre Access is authorized and no canonical phase is active.
    /// expected matches the top complete descriptor and ownership pair;
    /// destination is aligned, disjoint from stack storage, and large enough for expected.
    /// The transferred payload is not destroyed.
    ///
    /// Throws std::out_of_range on underflow and std::invalid_argument on source layout
    /// mismatch before accessing bytes; failure preserves the operand and destination.
    ///
    /// Complexity: O(1 + outer canonical bytes) time; no allocation or descriptor comparisons.
    void release(std::size_t floor, const value_type& expected, std::byte* destination);

public:
    /// Creates an empty reusable stack.
    ///
    /// Example: see the class example.
    raw_stack();
    /// Retires all live payloads and releases reserved storage.
    ///
    /// \pre No frame referring to this stack remains alive; no canonical phase is active.
    ///
    /// Complexity: O(live slots) time plus cleanup work; no traversal allocation.
    ///
    /// Example: see the class example.
    ~raw_stack();
    /// Disallows duplicated mutable ownership.
    raw_stack(const raw_stack&) = delete;
    /// Disallows copied assignment.
    raw_stack& operator=(const raw_stack&) = delete;
    /// Transfers other's storage and live operands, leaving other empty and reusable.
    ///
    /// Throws std::logic_error if other has an active frame or canonical phase;
    /// failure preserves other.
    ///
    /// Example: `raw_stack moved(std::move(stack));`.
    raw_stack(raw_stack&& other);
    /// Retires prior operands and transfers other, leaving other empty and reusable.
    ///
    /// Self-move is unchanged when access is permitted.
    ///
    /// Throws std::logic_error if either stack has an active frame or canonical phase;
    /// failure preserves both stacks.
    ///
    /// Complexity: O(prior live slots) time plus prior cleanup work.
    ///
    /// Example: `destination = std::move(stack);`.
    raw_stack& operator=(raw_stack&& other);
    /// Reserves bytes, alignment, and at least bytes live slots without changing the cursor.
    ///
    /// Preserves live operands and higher existing alignment.
    /// Zero-byte values may need reserve_depth in addition to byte reservation.
    ///
    /// Throws std::invalid_argument unless alignment is a nonzero power of two, and
    /// std::overflow_error for extent overflow.
    /// Throws std::logic_error if a frame or canonical phase is active.
    /// Failure preserves live operands and cursor; increased reservation may remain.
    ///
    /// Complexity: O(B + L) time on growth and O(Cb + Cs) reserved storage,
    /// using the class complexity terms; does not initialize spare bytes.
    ///
    /// Example: `stack.reserve(1024, 256);`.
    void reserve(std::size_t bytes, std::size_t alignment);
    /// Reserves bookkeeping for count simultaneously live operands, including zero-byte units.
    ///
    /// Preserves live operands, byte cursor, and existing reservation.
    ///
    /// Throws std::logic_error if a frame or canonical phase is active.
    /// Failure preserves live operands and cursor.
    ///
    /// Complexity: O(live slots) time and O(resulting slot capacity) storage on growth.
    ///
    /// Example: `stack.reserve_depth(100);`.
    void reserve_depth(std::size_t count);
    /// Retires every live operand while retaining reusable storage.
    ///
    /// Throws std::logic_error if a frame or canonical phase is active.
    ///
    /// Complexity: O(live slots) time plus cleanup work, without traversal allocation.
    ///
    /// Example: `stack.clear();`.
    void clear();
    /// Returns the number of live operands, excluding spare initialized bytes.
    ///
    /// Example: `raw_stack{}.depth() == 0`.
    std::size_t depth() const noexcept;
    /// Returns the active byte cursor, including padding preceding live operands.
    ///
    /// Example: `raw_stack{}.cursor() == 0`.
    std::size_t cursor() const noexcept;
    /// Appends a canonical copy of a concrete leaf or packed composite.
    ///
    /// Constructs no scalar public value boxes and does not acquire leaf pointee ownership.
    ///
    /// Throws std::logic_error if a frame or canonical phase is active;
    /// failure preserves live operands and cursor, but reservation may grow.
    ///
    /// Complexity: native construction work plus growth time; constructed payload and
    /// growth storage, plus adapter auxiliary space.
    ///
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
    /// Removes and returns an independent concrete copy of the top operand.
    ///
    /// Leaf pointers do not acquire pointee ownership.
    ///
    /// Throws std::out_of_range on underflow, std::invalid_argument on complete type
    /// mismatch, and std::logic_error if a frame or canonical phase is active.
    /// Failure preserves the operand.
    ///
    /// Complexity: native read and cleanup work plus expected O(P + E + S) comparison
    /// time; native result storage and adapter auxiliary space plus O(P + E) comparison
    /// auxiliary space. P, E, and S count visited descriptor pairs, edges, and compared
    /// name bytes; shared plans skip traversal.
    ///
    /// Example: `stack.push(true); auto flag = stack.pop<bool>();`.
    template <class T>
    T pop() {
        require_access(nullptr);
        auto result = read_value<T>(0);
        truncate(depth() - 1);
        return result;
    }
    /// Clones a public owner into canonical stack storage.
    ///
    /// Leaves source unchanged, preserving value's reference semantics for callable components.
    ///
    /// Throws std::logic_error if a frame or canonical phase is active;
    /// failure preserves source, live operands, and cursor, but reservation may grow.
    ///
    /// Complexity: payload clone work plus growth time; cloned payload and growth storage.
    ///
    /// Example: `stack.push_clone(value::from(std::vector{2.0}));`.
    void push_clone(const value& source);
    /// Transfers canonical ownership, leaving source unit after success.
    ///
    /// Throws std::logic_error if a frame or canonical phase is active;
    /// failure preserves source, live operands, and cursor, but reservation may grow.
    ///
    /// Complexity: O(1 + outer canonical bytes) time plus growth costs.
    ///
    /// Example: `auto v = value::from(true); stack.push_take(v);`.
    void push_take(value& source);
    /// Removes the top operand and transfers its canonical ownership to the returned value.
    ///
    /// Throws std::out_of_range on underflow and std::logic_error if a frame or canonical
    /// phase is active.
    /// Failure preserves the stack.
    ///
    /// Complexity: O(1 + outer canonical bytes) time and result storage.
    ///
    /// Example: `auto result = stack.pop_value();`.
    value pop_value();
};

/// @}
} // namespace cel
#endif
