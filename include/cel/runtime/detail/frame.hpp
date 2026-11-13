// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_DETAIL_FRAME_HPP
#define CEL_RUNTIME_DETAIL_FRAME_HPP
#include <cel/runtime/raw_stack.hpp>

namespace cel::detail {
/// A borrowed complete descriptor and initialized canonical value.
///
/// \pre Descriptor and source storage outlive every use. Bytes contain the complete
/// canonical type and ownership pair and do not alias storage mutated by the receiving
/// evaluation. The source stays unchanged during reads.
/// Copies retain the same borrows, not ownership. Destruction does not destroy the source.
/// Native leaves supply canonical bytes directly; native containers and callable objects do not.
///
/// Borrow a live leaf and extract a copy:
/// @code
/// double n = 2.0;
/// auto type = cel::value_type::leaf<double>();
/// cel::detail::borrowed_value input{type, reinterpret_cast<const std::byte*>(&n)};
/// auto copy = input.as<double>(); // 2.0; n remains owned by the caller.
/// @endcode
struct borrowed_value {
    const value_type& type; ///< Borrows live immutable canonical metadata.
    const std::byte* data;  ///< Borrows canonical bytes; null is valid for any zero-byte layout.
private:
    /// Checks concrete shape/operation compatibility before safe extraction.
    ///
    /// Throws std::invalid_argument for complete type mismatch or missing/misaligned
    /// nonempty source bytes.
    ///
    /// Complexity: expected O(P + E + S) time and O(P + E) auxiliary space for visited
    /// descriptor pairs P, edges E, and compared name bytes S; shared plans skip traversal.
    void require_type(const value_type& expected) const;

public:
    /// Returns an independent concrete copy of the borrowed value.
    ///
    /// Retains no borrow in an owning vector/tuple result; leaf pointers do not acquire ownership.
    ///
    /// Throws std::invalid_argument on a wrong complete type or absent/misaligned nonempty bytes.
    ///
    /// Complexity: native read work, including logical zero-byte elements and first-use
    /// descriptor construction, plus expected O(P + E + S) comparison time; native result
    /// storage and nesting-dependent adapter auxiliary space plus O(P + E) comparison
    /// auxiliary space. P, E, and S count visited descriptor pairs, edges, and compared
    /// name bytes; shared plans skip comparison traversal.
    ///
    /// Example: `auto n = input.as<double>();`.
    template <class T>
    T as() const {
        const auto expected = detail::concrete_adapter<T>::type();
        require_type(expected);
        return detail::concrete_adapter<T>::read(type, data);
    }
};

/// A borrowed evaluation scope with explicit arguments and optional client context.
///
/// Destruction retires only its suffix, restores the checkpoint byte cursor and depth,
/// and preserves the live caller prefix without rolling back client callback effects.
/// Frames nest in lexical LIFO order; stack and argument storage outlive the frame.
/// Only the innermost frame mutates the stack; context grants no mutable ownership.
/// Keep input owners and descriptors alive and unchanged during reads; keep a context object
/// alive for every use of its pointer. A frame retains spans, not copies of arguments.
/// The stack requires one exclusive evaluator, not concurrent independent access.
///
/// Construction, mutation, and execution throw std::logic_error on canonical-phase
/// reentry; argument views remain borrowed.
///
/// Complexity terms for growth, native adapters, payload work, and cleanup follow raw_stack.
///
/// Example: `raw_stack s; frame f(s); f.push(2.0); auto n = f.pop<double>();`.
class frame {
    // [knowledge] _checkpoint protects both live owners and their preceding padding:
    // each slot's previous cursor restores the byte origin when its suffix retires.
    /// Borrows the stack for this scope's lifetime.
    raw_stack* _stack;
    /// Borrows the enclosing scope until this scope exits.
    frame* _parent;
    /// Records the protected logical prefix depth.
    std::size_t _checkpoint;
    /// Borrows the supplied public argument owners.
    std::span<const value> _arguments;
    /// Borrows the supplied canonical argument descriptors and bytes.
    std::span<const borrowed_value> _canonical_arguments;
    /// Borrows optional caller context without granting mutable ownership.
    const void* _context;
    friend class raw_segment;
    friend class raw_stack;
    friend struct detail::frame_access;

    /// Checks this scope's exclusive mutation access.
    ///
    /// Throws std::logic_error when a nested frame or canonical phase is active.
    void require_active() const;
    /// Retires this scope's suffix and restores its checkpoint cursor.
    ///
    /// \pre This is the innermost frame and no canonical phase is active.
    /// Preserves the live prefix and reservation without allocation or
    /// nesting-dependent runtime cleanup stack.
    ///
    /// Complexity: O(suffix slots) time plus cleanup work.
    void reset() noexcept;
    /// Reads a trusted stored instruction operand into a concrete local value.
    ///
    /// \pre The instruction's complete type and ownership signature matches the operand;
    /// a canonical acquisition phase keeps its bytes stable during the read.
    ///
    /// Throws std::out_of_range on underflow and std::invalid_argument on layout mismatch.
    ///
    /// Complexity: native read work, result storage, and adapter auxiliary space.
    template <class T>
    T read_operand(std::size_t from_top, const value_type& expected) const {
        return detail::concrete_adapter<T>::read(
            expected, _stack->borrow_trusted(_checkpoint, from_top, expected));
    }
    /// Returns independent concrete operands in source order without consuming them.
    ///
    /// \pre Complete instruction type and ownership signatures match the frame suffix.
    /// Canonical bytes stay stable during acquisition.
    ///
    /// Throws std::logic_error if a nested frame or canonical phase is active,
    /// std::out_of_range on underflow, and std::invalid_argument on layout mismatch.
    /// Failure preserves live operands.
    /// Failed acquisition releases earlier temporary arguments before restoring the phase.
    ///
    /// Complexity: summed native read work, concrete result storage, and adapter auxiliary space.
    template <class... T, std::size_t... I>
    std::tuple<T...> acquire_operands(const std::array<value_type, sizeof...(T)>& types,
                                      std::index_sequence<I...>) const {
        require_active();
        raw_stack::phase_guard phase(*_stack);
        return {read_operand<T>(sizeof...(T) - 1 - I, types[I])...};
    }
    /// Removes consumed operands above the checkpoint.
    ///
    /// \pre This is the innermost frame and no canonical phase is active.
    ///
    /// Throws std::out_of_range on underflow without changing the suffix.
    ///
    /// Complexity: O(count) time plus cleanup work; no traversal allocation.
    void discard(std::size_t count);
    /// Appends a canonical copy of source using the supplied descriptor.
    ///
    /// \pre type is the complete canonical descriptor and ownership pair for T;
    /// source does not borrow potentially relocated stack bytes.
    ///
    /// Throws std::logic_error if a nested frame or canonical phase is active.
    /// Failure preserves live operands and cursor; reservation may grow.
    ///
    /// Complexity: native construction work plus growth time; constructed payload and
    /// growth storage, plus adapter auxiliary space.
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
    /// Enters a scope protecting the live stack prefix and borrowing arguments and context.
    ///
    /// Throws std::logic_error if a canonical ownership phase is active.
    /// Failure leaves live operands, cursor, and the enclosing frame unchanged.
    ///
    /// Example: `frame scope(stack, arguments, &client_context);`.
    explicit frame(raw_stack& stack,
                   std::span<const value> arguments = {},
                   const void* context = nullptr);
    /// Enters a prefix-protecting scope with borrowed canonical arguments and context.
    ///
    /// \pre Every descriptor and initialized source outlives the frame and nested uses;
    /// sources match their complete type and ownership pair, remain unchanged during reads,
    /// and do not alias the stack's mutable bytes.
    ///
    /// Throws std::invalid_argument for missing or misaligned nonempty source bytes.
    /// Throws std::logic_error if a canonical ownership phase is active.
    /// Failure leaves live operands, cursor, and the enclosing frame unchanged.
    ///
    /// Complexity: O(argument count) time; does not copy or allocate argument payloads.
    ///
    /// Example: `frame scope(stack, canonical_inputs, &client_context);`.
    frame(raw_stack& stack,
          std::span<const borrowed_value> arguments,
          const void* context = nullptr);
    /// Retires the acquired suffix and restores the enclosing frame.
    ///
    /// \pre Every nested frame has been destroyed; no canonical phase is active.
    /// Preserves the live prefix and its byte/depth origin.
    ///
    /// Complexity: O(suffix slots) time plus cleanup work, without traversal allocation
    /// or nesting-dependent runtime cleanup stack.
    ///
    /// Example: see the class example.
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
    ///
    /// Example: `frame f(stack); f.depth() == 0`.
    std::size_t depth() const noexcept;
    /// Returns the caller-owned stack for nested evaluation, not independent mutable ownership.
    ///
    /// Mutate through the innermost frame while any frame is active.
    ///
    /// Example: `frame nested(scope.stack(), scope.arguments(), scope.context());`.
    raw_stack& stack() const noexcept { return *_stack; }
    /// Borrows public input owners; empty when constructed with canonical pointer slots.
    ///
    /// The returned span borrows the constructor's input; this frame does not extend its lifetime.
    ///
    /// Example: `auto inputs = scope.arguments();`.
    std::span<const value> arguments() const noexcept { return _arguments; }
    /// Borrows pre-bound canonical inputs; empty when constructed with public value inputs.
    ///
    /// The returned span borrows the constructor's input; this frame does not extend its lifetime.
    ///
    /// Example: `frame nested(scope.stack(), scope.canonical_arguments(), scope.context());`.
    std::span<const borrowed_value> canonical_arguments() const noexcept {
        return _canonical_arguments;
    }
    /// Borrows one source-order argument without consuming or copying it.
    ///
    /// The descriptor and bytes remain caller-owned; keep their source alive and unchanged.
    ///
    /// Throws std::out_of_range if index is outside the supplied argument span.
    ///
    /// Example: `auto n = scope.argument(0).as<double>();`.
    borrowed_value argument(std::size_t index) const;
    /// Returns the explicitly borrowed context pointer, or null.
    ///
    /// The pointer remains caller-owned; the frame does not extend its lifetime.
    ///
    /// Example: `scope.context() == &client_context`.
    const void* context() const noexcept { return _context; }
    /// Appends a canonical clone of the indexed argument.
    ///
    /// Does not clone unused arguments or allocate scalar facade boxes.
    /// Mutable aggregate payloads are independent; callable components retain reference semantics.
    ///
    /// Throws std::out_of_range for an invalid index and std::logic_error for an active
    /// nested frame or canonical phase.
    /// Failure preserves arguments, live operands, and cursor; reservation may grow.
    ///
    /// Complexity: payload clone work plus growth time; cloned payload and growth storage.
    ///
    /// Example: `scope.push_argument(0);`.
    void push_argument(std::size_t index);
    /// Appends a canonical copy of a concrete operand to this scope.
    ///
    /// Leaf pointers do not acquire pointee ownership.
    ///
    /// Throws std::logic_error if a nested scope or canonical phase is active;
    /// failure preserves live operands and cursor, but reservation may grow.
    ///
    /// Complexity: native construction work plus growth time; constructed payload and
    /// growth storage, plus adapter auxiliary space.
    ///
    /// Example: `scope.push(2.0);`.
    template <class T>
    void push(const T& source) {
        push_result(detail::concrete_adapter<T>::type(), source);
    }
    /// Removes and returns an independent concrete copy of the top suffix operand.
    ///
    /// Never consumes the checkpoint prefix; leaf pointers do not acquire pointee ownership.
    ///
    /// Throws std::out_of_range on underflow, std::invalid_argument on complete type
    /// mismatch, and std::logic_error if a nested scope or canonical phase is active.
    /// Failure preserves the operand.
    ///
    /// Complexity: native read and cleanup work plus expected O(P + E + S) comparison
    /// time; native result storage and adapter auxiliary space plus O(P + E) comparison
    /// auxiliary space. P, E, and S count visited descriptor pairs, edges, and compared
    /// name bytes; shared plans skip traversal.
    ///
    /// Example: `auto n = scope.pop<double>();`.
    template <class T>
    T pop() {
        require_active();
        auto result = _stack->read_value<T>(_checkpoint);
        discard(1);
        return result;
    }
    /// Clones a public owner into this frame's suffix.
    ///
    /// Leaves source unchanged, preserving value's reference semantics for callable components.
    ///
    /// Throws std::logic_error if a nested scope or canonical phase is active;
    /// failure preserves source, live operands, and cursor, but reservation may grow.
    ///
    /// Complexity: payload clone work plus growth time; cloned payload and growth storage.
    ///
    /// Example: `scope.push_clone(value::from(true));`.
    void push_clone(const value& source);
    /// Appends source's canonical owner and leaves source unit.
    ///
    /// Throws std::logic_error if a nested scope or canonical phase is active;
    /// failure preserves source, live operands, and cursor, but reservation may grow.
    ///
    /// Complexity: O(1 + outer canonical bytes) time plus growth costs.
    ///
    /// Example: `scope.push_take(owner);`.
    void push_take(value& source);
    /// Removes the top suffix operand and transfers its ownership to the returned value.
    ///
    /// Throws std::out_of_range on underflow and std::logic_error if a nested scope or
    /// canonical phase is active.
    /// Failure preserves the operand.
    ///
    /// Complexity: O(1 + outer canonical bytes) time and result storage.
    ///
    /// Example: `auto result = scope.pop_value();`.
    value pop_value();
};
} // namespace cel::detail
#endif
