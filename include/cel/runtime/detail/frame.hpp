// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_DETAIL_FRAME_HPP
#define CEL_RUNTIME_DETAIL_FRAME_HPP
#include <cel/runtime/raw_stack.hpp>

namespace cel::detail {
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
} // namespace cel::detail
#endif
