// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_RAW_STACK_HPP
#define CEL_RUNTIME_RAW_STACK_HPP

#include <cel/runtime/value.hpp>
#include <cstddef>
#include <memory>
#include <span>
#include <utility>

namespace cel {
class frame;
class raw_segment;
namespace detail {
struct stack_state;
struct frame_access;
} // namespace detail

/// Borrows a complete descriptor and its initialized canonical representation.
/// Descriptor and bytes outlive every use; bytes match the complete type and ownership pair.
/// The bytes do not alias storage mutated by the receiving evaluation.
/// This view neither owns nor destroys its source and never relocates a public facade.
/// Example: `double n = 2.0; auto type = value_type::leaf<double>();
/// borrowed_value input{type, reinterpret_cast<const std::byte*>(&n)};`.
struct borrowed_value {
    const value_type& type; ///< Borrows live immutable canonical metadata.
    const std::byte* data;  ///< Borrows initialized canonical bytes; null is valid only for unit.
private:
    /// Checks concrete shape/operation compatibility before safe extraction.
    /// Complexity: O(compared shape size) time/space for distinct plans.
    void require_type(const value_type& expected) const;

public:
    /// Returns an independent concrete result without creating intermediate value facades.
    /// Throws invalid_argument on a wrong complete type or absent/misaligned bytes.
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
/// Only internal canonical carriers transfer bytewise; public facades never do.
/// Moving requires no active frames; the owner outlives every borrowed frame.
/// Exclusive access is required, including during nested evaluation.
/// Example: `raw_stack s; s.reserve(256, 256); s.push(2.0); auto n = s.pop<double>();`.
class raw_stack {
    std::unique_ptr<detail::stack_state> _state;
    friend class frame;
    friend struct detail::frame_access;

    /// Creates the state of a moved-from reusable stack if needed.
    detail::stack_state& state();
    /// Rejects mutation outside the innermost active frame.
    void require_access(const frame* owner) const;
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
    /// Retires all live payloads; no frame may outlive the stack.
    /// Complexity: O(slots + visited payload ownership paths).
    ~raw_stack();
    /// Disallows duplicated mutable ownership.
    raw_stack(const raw_stack&) = delete;
    /// Disallows copied assignment.
    raw_stack& operator=(const raw_stack&) = delete;
    /// Transfers storage and live slots; leaves other empty and reusable.
    /// Throws logic_error if other has active frames.
    raw_stack(raw_stack&& other);
    /// Releases prior payloads and transfers other; self-assignment is unchanged.
    /// Throws logic_error if either stack has active frames.
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
        auto result = detail::concrete_adapter<T>::read(
            detail::concrete_adapter<T>::type(), borrow(0, 0, detail::concrete_adapter<T>::type()));
        truncate(depth() - 1);
        return result;
    }
    /// Clones a public owner into canonical stack storage.
    /// Throws logic_error if a frame is active; failure preserves all operands.
    /// Complexity: O(cloned payload + retained bytes/slots on growth) time and space.
    /// Example: `stack.push_clone(value::from(std::vector{2.0}));`.
    void push_clone(const value& source);
    /// Transfers canonical ownership, leaving source unit after success.
    /// Throws logic_error if a frame is active; failure preserves source and stack.
    /// Complexity: O(outer bytes + retained bytes/slots on growth).
    /// Example: `auto v = value::from(true); stack.push_take(v);`.
    void push_take(value& source);
    /// Transfers the top canonical owner into a public facade.
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
    /// Checkpoints stack depth and explicitly borrows arguments and context.
    /// Example: `frame scope(stack, arguments, &client_context);`.
    explicit frame(raw_stack& stack,
                   std::span<const value> arguments = {},
                   const void* context = nullptr);
    /// Checkpoints a stack and borrows pre-bound canonical argument slots and context.
    /// Precondition: every descriptor/initialized source outlives the frame and its nested uses;
    /// sources do not alias the stack's mutable bytes.
    /// Throws invalid_argument for missing or misaligned nonempty source bytes.
    /// Complexity: O(argument count) time; does not copy or allocate argument payloads.
    /// Example: `frame scope(stack, canonical_inputs, &client_context);`.
    frame(raw_stack& stack,
          std::span<const borrowed_value> arguments,
          const void* context = nullptr);
    /// Retires the acquired suffix and restores the enclosing frame.
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
    /// Returns this scope's live operand depth above its checkpoint.
    /// Example: `frame f(stack); f.depth() == 0`.
    std::size_t depth() const noexcept;
    /// Borrows the caller-owned stack for nested evaluation.
    /// Example: `frame nested(scope.stack(), scope.arguments(), scope.context());`.
    raw_stack& stack() const noexcept { return *_stack; }
    /// Borrows public input owners; empty when constructed with canonical pointer slots.
    /// Example: `auto inputs = scope.arguments();`.
    std::span<const value> arguments() const noexcept { return _arguments; }
    /// Borrows pre-bound canonical inputs; empty when constructed with public value inputs.
    /// Example: `frame nested(scope.stack(), scope.canonical_arguments(), scope.context());`.
    std::span<const borrowed_value> canonical_arguments() const noexcept {
        return _canonical_arguments;
    }
    /// Borrows one argument; throws out_of_range on malformed external index.
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
        auto result = detail::concrete_adapter<T>::read(
            detail::concrete_adapter<T>::type(),
            _stack->borrow(_checkpoint, 0, detail::concrete_adapter<T>::type()));
        discard(1);
        return result;
    }
    /// Clones a public owner into this frame's suffix.
    /// Throws logic_error if a nested scope is active; failure preserves live operands.
    /// Complexity: O(cloned payload + retained bytes/slots on growth) time and space.
    /// Example: `scope.push_clone(value::from(true));`.
    void push_clone(const value& source);
    /// Transfers canonical bytes from source, leaving it unit.
    /// Throws logic_error if a nested scope is active; failure preserves source and stack.
    /// Complexity: O(outer bytes + retained bytes/slots on growth).
    /// Example: `scope.push_take(owner);`.
    void push_take(value& source);
    /// Transfers the top suffix operand to a public owner.
    /// Throws out_of_range on underflow and logic_error if a nested scope is active.
    /// Failure preserves the operand.
    /// Complexity: O(outer bytes) time and space.
    /// Example: `auto result = scope.pop_value();`.
    value pop_value();
};
} // namespace cel
#endif
