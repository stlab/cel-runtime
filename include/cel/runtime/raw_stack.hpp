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
class frame;
class raw_segment;
namespace detail {
struct stack_state;
struct frame_access;
} // namespace detail

/// Borrows a complete descriptor and its initialized canonical representation.
/// Precondition: descriptor and bytes outlive every use; bytes match the complete type and
/// ownership pair and do not alias storage mutated by the receiving evaluation.
/// Copies retain the same borrows, not ownership. Destruction does not destroy the source.
/// Native leaves supply canonical bytes directly; native containers and callable objects do not.
///
/// Borrow a live leaf and extract a copy:
/// @code
/// double n = 2.0;
/// auto type = cel::value_type::leaf<double>();
/// cel::borrowed_value input{type, reinterpret_cast<const std::byte*>(&n)};
/// auto copy = input.as<double>(); // 2.0; n remains owned by the caller.
/// @endcode
struct borrowed_value {
    const value_type& type; ///< Borrows live immutable canonical metadata.
    const std::byte* data;  ///< Borrows canonical bytes; null is valid for any zero-byte layout.
private:
    /// Checks concrete shape/operation compatibility before safe extraction.
    /// Complexity: O(compared shape size) time/space for distinct plans.
    void require_type(const value_type& expected) const;

public:
    /// Returns a concrete leaf, vector, or tuple copy from compatible canonical bytes.
    /// Retains no borrow in an owning vector/tuple result; leaf pointers do not acquire ownership.
    /// Throws std::invalid_argument on a wrong complete type or absent/misaligned nonempty bytes.
    /// Complexity: O(payload + compared shape size) time and result space.
    /// Example: `auto n = input.as<double>();`.
    template <class T>
    T as() const {
        const auto expected = detail::concrete_adapter<T>::type();
        require_type(expected);
        return detail::concrete_adapter<T>::read(type, data);
    }
};

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
    friend class frame;
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
    void require_access(const frame* owner) const;
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

/// Borrows one explicit evaluation scope, argument span, and optional client context.
/// Destruction retires its suffix and preserves its checkpoint's live prefix.
/// Frames are nonmovable and nested in lexical LIFO order; stack and arguments outlive them.
/// Only the innermost frame mutates the stack; context grants no mutable ownership.
/// Keep input owners and descriptors alive and unchanged during reads; keep a context object
/// alive for every use of its pointer. A frame retains spans, not copies of arguments.
/// Construction throws logic_error while the stack's canonical phase gate is held.
/// Mutating calls and execution also reject phase reentry; argument views remain borrowed.
/// Example: `raw_stack s; frame f(s); f.push(2.0); auto n = f.pop<double>();`.
class frame {
    raw_stack* _stack;
    frame* _parent;
    std::size_t _checkpoint;
    std::span<const value> _arguments;
    std::span<const borrowed_value> _canonical_arguments;
    const void* _context;
    friend class raw_segment;
    friend class raw_stack;
    friend struct detail::frame_access;

    /// Checks lexical mutation exclusivity.
    void require_active() const;
    /// Retires only this frame's suffix without allocating.
    /// Complexity: O(suffix slots + visited payload ownership paths).
    void reset() noexcept;
    /// Reads a trusted stored instruction operand into a concrete local value.
    /// Precondition: the instruction's complete operand signature matches the live suffix.
    /// Complexity: O(concrete payload size) time and result space.
    template <class T>
    T read_operand(std::size_t from_top, const value_type& expected) const {
        return detail::concrete_adapter<T>::read(
            expected, _stack->borrow_trusted(_checkpoint, from_top, expected));
    }
    /// Acquires ordered concrete operands under one stable-byte phase.
    /// Precondition: complete instruction signatures match the frame suffix.
    /// Failed acquisition releases earlier temporary arguments before restoring the phase.
    /// Complexity: O(operand payload size) time and concrete argument space.
    template <class... T, std::size_t... I>
    std::tuple<T...> acquire_operands(const std::array<value_type, sizeof...(T)>& types,
                                      std::index_sequence<I...>) const {
        require_active();
        raw_stack::phase_guard phase(*_stack);
        return {read_operand<T>(sizeof...(T) - 1 - I, types[I])...};
    }
    /// Removes consumed operands above the checkpoint.
    /// Throws out_of_range on underflow.
    /// Complexity: O(count + visited payload ownership paths).
    void discard(std::size_t count);
    /// Constructs a typed result using retained instruction metadata.
    /// Precondition: type is the complete canonical descriptor for T.
    /// Complexity: O(result construction + retained bytes/slots on growth).
    template <class T>
    void push_result(const value_type& type, const T& source) {
        require_active();
        _stack->push_written(
            type, &source, [](const void* input, const value_type& shape, std::byte* destination) {
                detail::concrete_adapter<T>::write(*static_cast<const T*>(input), shape,
                                                   destination);
            });
    }

public:
    /// Checkpoints stack depth and borrows public arguments and an optional context pointer.
    /// Defaults to no arguments and null context; entering a nested frame protects the live prefix.
    /// Throws logic_error if a canonical ownership phase is active.
    /// Example: `frame scope(stack, arguments, &client_context);`.
    explicit frame(raw_stack& stack,
                   std::span<const value> arguments = {},
                   const void* context = nullptr);
    /// Checkpoints a stack and borrows pre-bound canonical argument slots and context.
    /// Precondition: every descriptor/initialized source outlives the frame and its nested uses;
    /// sources do not alias the stack's mutable bytes.
    /// Throws invalid_argument for missing or misaligned nonempty source bytes.
    /// Throws logic_error if a canonical ownership phase is active.
    /// Complexity: O(argument count) time; does not copy or allocate argument payloads.
    /// Example: `frame scope(stack, canonical_inputs, &client_context);`.
    frame(raw_stack& stack,
          std::span<const borrowed_value> arguments,
          const void* context = nullptr);
    /// Retires the acquired suffix and restores the enclosing frame.
    /// Precondition: every nested frame has already been destroyed.
    /// Complexity: O(suffix slots + visited payload ownership paths), without allocating.
    ~frame();
    /// Disallows duplicated scope ownership.
    frame(const frame&) = delete;
    /// Disallows reassigned scope ownership.
    frame& operator=(const frame&) = delete;
    /// Disallows relocating an active lexical scope.
    frame(frame&&) = delete;
    /// Disallows move assignment of active scopes.
    frame& operator=(frame&&) = delete;
    /// Returns the live stack depth above this scope's checkpoint, including any nested suffix.
    /// Example: `frame f(stack); f.depth() == 0`.
    std::size_t depth() const noexcept;
    /// Returns the caller-owned stack for nested evaluation, not independent mutable ownership.
    /// Mutate through the innermost frame while any frame is active.
    /// Example: `frame nested(scope.stack(), scope.arguments(), scope.context());`.
    raw_stack& stack() const noexcept { return *_stack; }
    /// Borrows public input owners; empty when constructed with canonical pointer slots.
    /// The returned span borrows the constructor's input; this frame does not extend its lifetime.
    /// Example: `auto inputs = scope.arguments();`.
    std::span<const value> arguments() const noexcept { return _arguments; }
    /// Borrows pre-bound canonical inputs; empty when constructed with public value inputs.
    /// The returned span borrows the constructor's input; this frame does not extend its lifetime.
    /// Example: `frame nested(scope.stack(), scope.canonical_arguments(), scope.context());`.
    std::span<const borrowed_value> canonical_arguments() const noexcept {
        return _canonical_arguments;
    }
    /// Borrows one source-order argument without consuming or copying it.
    /// The descriptor and bytes remain caller-owned; keep their source alive and unchanged.
    /// Throws std::out_of_range if index is outside the supplied argument span.
    /// Example: `auto n = scope.argument(0).as<double>();`.
    borrowed_value argument(std::size_t index) const;
    /// Returns the explicitly borrowed context pointer, or null.
    /// Example: `scope.context() == &client_context`.
    const void* context() const noexcept { return _context; }
    /// Clones one emitted argument read directly into the canonical suffix.
    /// Does not clone unused arguments or allocate scalar facade boxes.
    /// Throws out_of_range for an invalid index and logic_error for an active nested frame.
    /// Failure preserves existing operands.
    /// Complexity: O(cloned payload + retained bytes/slots on growth) time and space.
    /// Example: `scope.push_argument(0);`.
    void push_argument(std::size_t index);
    /// Writes a concrete operand in the innermost scope.
    /// Throws logic_error if a nested scope is active; failure preserves live operands.
    /// Complexity: O(payload construction + retained bytes/slots on growth).
    /// Example: `scope.push(2.0);`.
    template <class T>
    void push(const T& source) {
        push_result(detail::concrete_adapter<T>::type(), source);
    }
    /// Returns an independent top value without consuming the checkpoint prefix.
    /// Throws out_of_range on underflow, invalid_argument on complete type mismatch,
    /// and logic_error if a nested scope is active. Failure preserves the operand.
    /// Complexity: O(result payload + compared shape size) time and result space.
    /// Example: `auto n = scope.pop<double>();`.
    template <class T>
    T pop() {
        require_active();
        auto result = _stack->read_value<T>(_checkpoint);
        discard(1);
        return result;
    }
    /// Clones a public owner into this frame's suffix.
    /// Leaves source unchanged, preserving value's reference semantics for callable components.
    /// Throws logic_error if a nested scope is active; failure preserves live operands.
    /// Complexity: O(cloned payload + retained bytes/slots on growth) time and space.
    /// Example: `scope.push_clone(value::from(true));`.
    void push_clone(const value& source);
    /// Transfers canonical bytes from source, leaving it unit.
    /// Throws logic_error if a nested scope is active; failure preserves source and stack.
    /// Complexity: O(outer bytes + retained bytes/slots on growth).
    /// Example: `scope.push_take(owner);`.
    void push_take(value& source);
    /// Removes the top suffix operand and transfers its ownership to the returned value.
    /// Throws out_of_range on underflow and logic_error if a nested scope is active.
    /// Failure preserves the operand.
    /// Complexity: O(outer bytes) time and space.
    /// Example: `auto result = scope.pop_value();`.
    value pop_value();
};
} // namespace cel
#endif
