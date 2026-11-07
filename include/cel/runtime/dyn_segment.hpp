// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_DYN_SEGMENT_HPP
#define CEL_RUNTIME_DYN_SEGMENT_HPP
#include <cel/runtime/raw_segment.hpp>
#include <concepts>
#include <new>
#include <vector>

namespace cel {
class compiled_segment;
class prepared_raw_segment;
template <class R, class... Args>
class prepared_segment;

/// Describes total caller storage and slot capacity for one prefixed execution.
/// Example: `stack.reserve(r.bytes.size, r.bytes.alignment); stack.reserve_depth(r.slots);`.
struct stack_reservation {
    layout bytes;      ///< Gives total byte capacity and required alignment.
    std::size_t slots; ///< Gives total live-slot capacity, including the caller prefix.
};

/// Describes statically known canonical frame storage for a frozen ordinary program.
/// Extent and slots are peak simultaneous requirements, not cumulative instruction costs.
/// Composite payload allocations, concrete callback temporaries, and arbitrary callback or
/// nested external stack work are not inferred. Result destinations remain caller-owned.
/// Example: `auto r = program.requirements().for_prefix(stack.cursor(), stack.depth());`.
struct stack_requirements {
    std::size_t alignment = 1;    ///< Gives maximum intermediate canonical alignment.
    std::size_t frame_extent = 0; ///< Gives peak canonical extent for a zero-cursor frame.
    std::size_t live_slots = 0;   ///< Gives peak simultaneously live frame operands.

    /// Returns checked conservative total capacities for an existing caller prefix.
    /// Rounds cursor up to alignment, then adds frame_extent; adds depth to live_slots.
    /// Existing storage alignment is retained by raw_stack::reserve, never narrowed.
    /// Throws invalid_argument for invalid alignment and overflow_error for either sum.
    /// Reserve before entering a frame, using cursor/depth measured without intervening mutation.
    /// No allocation or signature traversal occurs.
    /// Example: `auto r = requirements.for_prefix(stack.cursor(), stack.depth());
    /// stack.reserve(r.bytes.size, r.bytes.alignment); stack.reserve_depth(r.slots);`.
    stack_reservation for_prefix(std::size_t cursor, std::size_t depth) const;
};

namespace detail {
struct frozen_program;
/// Selects native leaf boundaries rather than unspecified standard composite layouts.
template <class T>
inline constexpr bool native_leaf = true;
/// Selects the trusted vector adapter.
template <class T, class A>
inline constexpr bool native_leaf<std::vector<T, A>> = false;
/// Selects the trusted tuple adapter.
template <class... T>
inline constexpr bool native_leaf<std::tuple<T...>> = false;
/// Describes a uniquely declared callback signature without accepting operand conversions.
template <class T>
struct callback_signature {};
/// Describes ordinary function pointers.
template <class R, class... A>
struct callback_signature<R (*)(A...)> {
    using arguments = std::tuple<std::remove_cvref_t<A>...>; ///< Gives exact operand identities.
};
/// Describes nonthrowing function pointers.
template <class R, class... A>
struct callback_signature<R (*)(A...) noexcept> : callback_signature<R (*)(A...)> {};
/// Describes const callback members.
template <class C, class R, class... A>
struct callback_signature<R (C::*)(A...) const> : callback_signature<R (*)(A...)> {};
/// Describes nonthrowing const callback members.
template <class C, class R, class... A>
struct callback_signature<R (C::*)(A...) const noexcept> : callback_signature<R (*)(A...)> {};
/// Describes callbacks callable through a const lvalue.
template <class C, class R, class... A>
struct callback_signature<R (C::*)(A...) const&> : callback_signature<R (*)(A...)> {};
/// Describes nonthrowing callbacks callable through a const lvalue.
template <class C, class R, class... A>
struct callback_signature<R (C::*)(A...) const & noexcept> : callback_signature<R (*)(A...)> {};
/// Selects a member template by an exact invocation signature, not explicit type arguments.
/// Uniform value, const-reference, and forwarding-reference parameters are supported;
/// other parameter patterns use with_signature's explicit exact selection.
template <class F, class R, class... A>
consteval auto generic_member() {
    if constexpr (requires { static_cast<R (F::*)(A...) const>(&F::template operator()); })
        return static_cast<R (F::*)(A...) const>(&F::template operator());
    else if constexpr (requires { static_cast<R (F::*)(A...) const&>(&F::template operator()); })
        return static_cast<R (F::*)(A...) const&>(&F::template operator());
    else if constexpr (requires {
                           static_cast<R (F::*)(const A&...) const>(&F::template operator());
                       })
        return static_cast<R (F::*)(const A&...) const>(&F::template operator());
    else if constexpr (requires {
                           static_cast<R (F::*)(const A&...) const&>(&F::template operator());
                       })
        return static_cast<R (F::*)(const A&...) const&>(&F::template operator());
    else if constexpr (requires { static_cast<R (F::*)(A&&...) const>(&F::template operator()); })
        return static_cast<R (F::*)(A&&...) const>(&F::template operator());
    else if constexpr (requires { static_cast<R (F::*)(A&&...) const&>(&F::template operator()); })
        return static_cast<R (F::*)(A&&...) const&>(&F::template operator());
}
/// Rejects conversions using declared signatures and deduction-proven generic signatures.
template <class F, class R, class... A>
consteval bool exact_callback() {
    if constexpr (!std::is_invocable_v<const F&, A...>)
        return false;
    else {
        constexpr bool result_matches =
            std::same_as<R, std::remove_cvref_t<std::invoke_result_t<const F&, A...>>>;
        if constexpr (requires { typename callback_signature<F>::arguments; })
            return result_matches &&
                   std::same_as<typename callback_signature<F>::arguments, std::tuple<A...>>;
        else if constexpr (requires {
                               typename callback_signature<decltype(&F::operator())>::arguments;
                           })
            return result_matches &&
                   std::same_as<typename callback_signature<decltype(&F::operator())>::arguments,
                                std::tuple<A...>>;
        else {
            using member =
                decltype(generic_member<F, std::invoke_result_t<const F&, A...>, A...>());
            if constexpr (std::is_void_v<member>)
                return false;
            else
                return result_matches && std::is_invocable_v<member, const F&, A...>;
        }
    }
}
/// Describes whether an explicit exact member signature can be selected.
template <class Signature, class F>
struct signature_binding {
    static constexpr bool available = false; ///< Rejects unsupported signature descriptions.
};
/// Selects an exact const callback member, never an implicitly converting invocation.
template <class R, class... A, class F>
struct signature_binding<R(A...), F> {
    /// Returns the exact selected member pointer, or void when selection is unavailable.
    static consteval auto select() {
        if constexpr (requires { static_cast<R (F::*)(A...) const>(&F::operator()); })
            return static_cast<R (F::*)(A...) const>(&F::operator());
        else if constexpr (requires { static_cast<R (F::*)(A...) const&>(&F::operator()); })
            return static_cast<R (F::*)(A...) const&>(&F::operator());
    }
    static constexpr bool available = !std::is_void_v<decltype(select())>;
    ///< Reports successful exact member selection.
};
/// Owns a callback with a uniquely selected exact public invocation signature.
template <class Signature, class F>
class signature_callback;
/// Invokes the selected member directly rather than redispatching an ambiguous overload set.
template <class R, class... A, class F>
class signature_callback<R(A...), F> {
    F _callback;

public:
    /// Transfers the normally constructed callback object.
    /// Complexity: O(callback construction).
    explicit signature_callback(F callback) : _callback(std::move(callback)) {}
    /// Invokes exactly the previously selected const member.
    /// Complexity: O(callback work).
    R operator()(A... arguments) const {
        return std::invoke(signature_binding<R(A...), F>::select(), _callback,
                           std::forward<A>(arguments)...);
    }
};
/// Retains one emitted positional read and its complete frozen clone witness.
struct argument_read {
    std::size_t index; ///< Gives the caller's source-order position.
    value_type type;   ///< Retains complete shape and ownership.
};
/// Stages checked canonical accounting until the corresponding opcode is published.
struct construction_effect {
    std::size_t cursor;              ///< Gives the resulting zero-origin frame cursor.
    stack_requirements requirements; ///< Gives the staged peak requirements.
};
} // namespace detail

/// Selects an exact const callback member from an ambiguous or uninspectable overload set.
/// Signature includes actual parameter and return reference qualifiers; no conversion
/// creates a signature that the callback does not declare. The wrapper owns F normally.
/// Generic callbacks that cannot prove their instantiated parameter identities use this
/// explicit selection. Function pointers can instead be supplied directly with an exact cast.
/// Complexity: O(callback construction) time and capture space.
/// Example: `builder.op1<double, double>(with_signature<double(double)>(overloaded{}));`.
template <class Signature, class F>
requires(detail::signature_binding<Signature, F>::available) auto with_signature(F callback) {
    return detail::signature_callback<Signature, F>(std::move(callback));
}

/// Checks stack effects while constructing a move-only ordinary operation sequence.
/// Operations consume older-first exact operands; exceptions replace Result-shaped transport.
/// Successful finish freezes captures and descriptors independently of builder lifetime.
/// Example: `dyn_segment b; b.just(value::from(2.0)); auto p = std::move(b).finish();`.
class dyn_segment {
    raw_segment _body;
    std::vector<value_type> _stack;
    std::vector<detail::argument_read> _reads;
    std::vector<std::size_t> _end_cursors; ///< Mirrors canonical cursor ends in _stack order.
    stack_requirements _requirements;      ///< Retains committed intermediate storage peaks.
    /// Validates the ordered suffix and reserves metadata before opcode publication.
    /// Throws invalid_argument for underflow, incompatible operands, or absent ownership.
    /// Complexity: O(arity + distinct compared metadata pairs) time and auxiliary space,
    /// plus O(current stack depth) metadata transfer on capacity growth.
    detail::construction_effect check_operation(std::span<const value_type> inputs,
                                                std::span<const value_type> outputs);
    /// Publishes a previously validated logical stack effect without allocation.
    /// Precondition: check_operation supplied effect and body publication succeeded;
    /// outputs contains at most one result.
    /// Complexity: O(consumed operand count).
    void apply_operation(std::size_t consumed,
                         std::span<const value_type> outputs,
                         const detail::construction_effect& effect) noexcept;

public:
    /// Creates an empty checked stack.
    /// Example: `dyn_segment builder;`.
    dyn_segment() = default;
    /// Disallows copying mutable construction ownership.
    dyn_segment(const dyn_segment&) = delete;
    /// Disallows copied assignment.
    dyn_segment& operator=(const dyn_segment&) = delete;
    /// Transfers construction ownership.
    dyn_segment(dyn_segment&&) noexcept = default;
    /// Transfers construction ownership, releasing prior captures and metadata.
    /// Complexity: O(old captures and metadata).
    dyn_segment& operator=(dyn_segment&&) noexcept = default;
    /// Emits an owned clone of a borrowed positional input.
    /// Repeated declarations must have identical complete shapes and ownership plans.
    /// Throws invalid_argument for incompatible repeated declarations or missing clone capability.
    /// Complexity: O(emitted read count + compared metadata) construction time,
    /// plus O(current stack depth) metadata transfer on capacity growth.
    /// Example: `builder.push_argument(0, value_type::leaf<double>());`.
    void push_argument(std::size_t index, value_type type);
    /// Retains a literal owner and emits an independent clone on every execution.
    /// Throws invalid_argument if the complete value cannot be cloned.
    /// Complexity: O(metadata admission) time, plus O(current stack depth) metadata transfer
    /// on capacity growth; retains one owner.
    /// Example: `builder.just(value::from(true));`.
    void just(value literal);
    /// Emits an arbitrary-arity const callback with exact declared operands and result.
    /// R is a trusted concrete adapter type or void; void consumes without producing a value.
    /// Named callback parameters must match Args after cv/reference removal.
    /// Generic callbacks must expose a specialization with provably exact Args that
    /// deduction from its complete function-pointer signature also selects.
    /// Ambiguous/uninspectable callbacks require with_signature's exact member selection.
    /// Callbacks return R or a reference to R; no implicit result conversion is admitted.
    /// Callbacks may reference caller-owned state; freezing does not make it thread-safe.
    /// Throws invalid_argument for current stack mismatch before publishing an opcode.
    /// Complexity: O(arity + compared metadata + capture construction) time,
    /// plus O(current stack depth) metadata transfer on capacity growth.
    /// Example: `builder.op<double, double>([](double n) { return n * 2.0; });`.
    template <class R, class... Args, class F>
    requires(detail::exact_callback<F, R, Args...>()) void op(F callback) {
        static_assert((std::same_as<Args, std::remove_cvref_t<Args>> && ...));
        const std::array<value_type, sizeof...(Args)> inputs{
            detail::concrete_adapter<Args>::type()...};
        const auto outputs = [] {
            if constexpr (std::is_void_v<R>)
                return std::array<value_type, 0>{};
            else
                return std::array{detail::concrete_adapter<R>::type()};
        }();
        const auto effect = check_operation(inputs, outputs);
        _body.template push_op<R, Args...>(
            [callback = std::move(callback)](frame&, Args... operands) -> R {
                return std::invoke(callback, std::move(operands)...);
            });
        apply_operation(sizeof...(Args), outputs, effect);
    }
    /// Pushes a zero-operand result; follows op's validation and exception contract.
    /// Complexity: O(capture construction), plus O(current stack depth) on metadata growth.
    /// Example: `builder.op0<double>([] { return 2.0; });`.
    template <class R, class F>
    requires(detail::exact_callback<F, R>()) void op0(F callback) {
        op<R>(std::move(callback));
    }
    /// Replaces one exact operand; follows op's validation and exception contract.
    /// Complexity: O(compared metadata + capture construction).
    /// Example: `builder.op1<bool, bool>([](bool b) { return !b; });`.
    template <class A, class R, class F>
    requires(detail::exact_callback<F, R, A>()) void op1(F callback) {
        op<R, A>(std::move(callback));
    }
    /// Consumes two operands older-first; follows op's validation and exception contract.
    /// Complexity: O(compared metadata + capture construction).
    /// Example: `builder.op2<double, double, double>([](double a, double b) { return a - b; });`.
    template <class A, class B, class R, class F>
    requires(detail::exact_callback<F, R, A, B>()) void op2(F callback) {
        op<R, A, B>(std::move(callback));
    }
    /// Consumes three source-order operands; follows op's validation and exception contract.
    /// Complexity: O(compared metadata + capture construction).
    /// Example: `builder.op3<double, double, double, double>(
    /// [](double a, double b, double c) { return a + b + c; });`.
    template <class A, class B, class C, class R, class F>
    requires(detail::exact_callback<F, R, A, B, C>()) void op3(F callback) {
        op<R, A, B, C>(std::move(callback));
    }
    /// Freezes exactly one result, mapping an empty final stack to source unit.
    /// Throws invalid_argument for leftover operands instead of discarding them.
    /// Complexity: O(1), except allocation and source-unit emission.
    /// Example: `auto program = std::move(builder).finish();`.
    compiled_segment finish() &&;
};

/// Shares deliberately reference-semantic immutable code and frozen captures.
/// Handles are move-only; factories retain the engine after this handle is destroyed.
/// Captures are never copied as independent values; external referenced state stays caller-owned.
/// Example: `auto prepared = program.prepare<double, double>();`.
class compiled_segment {
    std::shared_ptr<const detail::frozen_program> _program;
    friend class dyn_segment;
    /// Retains a fully checked frozen engine.
    explicit compiled_segment(std::shared_ptr<const detail::frozen_program> program) noexcept;

public:
    /// Disallows misleading independent capture copies.
    compiled_segment(const compiled_segment&) = delete;
    /// Disallows copied assignment.
    compiled_segment& operator=(const compiled_segment&) = delete;
    /// Transfers the frozen handle.
    compiled_segment(compiled_segment&&) noexcept = default;
    /// Transfers the handle and releases prior code when uniquely owned.
    /// Complexity: O(prior captures and metadata on last release).
    compiled_segment& operator=(compiled_segment&&) noexcept = default;
    /// Returns the complete frozen result descriptor.
    /// Precondition: this handle has not been moved from.
    /// Example: `auto type = program.result_type();`.
    const value_type& result_type() const noexcept;
    /// Borrows frozen intermediate alignment, peak canonical extent, and live-slot depth.
    /// Precondition: this handle has not been moved from.
    /// Arbitrary callback/nested costs are not inferred; use for_prefix before reserving.
    /// Example: `auto r = program.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept;
    /// Borrows emitted read requirements; absent positional inputs are unused.
    /// Precondition: this handle has not been moved from.
    /// Example: `auto reads = program.argument_reads();`.
    std::span<const detail::argument_read> argument_reads() const noexcept;
    /// Validates all actual read witnesses before effects and returns an independent owner.
    /// Extra unused inputs are permitted and are not materialized.
    /// Throws invalid_argument for missing/incompatible read types or ownership.
    /// Callback/acquisition exceptions propagate after nonallocating suffix cleanup.
    /// Complexity: O(read validation + executed instructions + payload work) time and result space.
    /// Example: `auto result = program.execute(stack, inputs);`.
    value execute(raw_stack& stack, std::span<const value> arguments) const;
    /// Binds complete runtime shapes and ownership plans once.
    /// Throws invalid_argument for undeclared reads, incompatible types, or absent ownership.
    /// Complexity: O(arguments + distinct compared metadata pairs) time and binding space.
    /// Example: `auto bound = program.prepare_raw(types, program.result_type());`.
    prepared_raw_segment prepare_raw(std::span<const value_type> arguments,
                                     const value_type& result) const;
    /// Binds exact recursive concrete adapters once, admitting valid client leaves at roots.
    /// Throws invalid_argument for incompatible argument/result shapes or undeclared reads.
    /// Complexity: O(arity + distinct compared metadata pairs) time and binding space.
    /// Example: `auto bound = program.prepare<double, double>();`.
    template <class R, class... Args>
    prepared_segment<R, Args...> prepare() const;
};

/// Retains a move-only, reference-semantic frozen canonical execution binding.
/// No call compares descriptors, converts operands, or allocates scalar public owner boxes.
/// Referenced callback state remains caller-owned and supplies its own synchronization.
/// Example: `bound.call_into(stack, borrowed_inputs, result_bytes);`.
class prepared_raw_segment {
    std::shared_ptr<const detail::frozen_program> _program;
    std::vector<value_type> _arguments;
    value_type _result;
    std::vector<bool> _used;
    value_type _unit;
    friend class compiled_segment;
    template <class R, class... Args>
    friend class prepared_segment;
    /// Retains a validated frozen signature.
    /// Complexity: O(argument count) time and binding space.
    prepared_raw_segment(std::shared_ptr<const detail::frozen_program> program,
                         std::span<const value_type> arguments,
                         value_type result);

public:
    /// Disallows independent-value copy claims for captures.
    prepared_raw_segment(const prepared_raw_segment&) = delete;
    /// Disallows copied assignment.
    prepared_raw_segment& operator=(const prepared_raw_segment&) = delete;
    /// Transfers the frozen binding.
    prepared_raw_segment(prepared_raw_segment&&) noexcept = default;
    /// Transfers the binding and releases old metadata/code.
    /// Complexity: O(old argument metadata and last-owned captures).
    prepared_raw_segment& operator=(prepared_raw_segment&&) noexcept = default;
    /// Borrows prepared source-order argument descriptors.
    /// Example: `auto arity = bound.argument_types().size();`.
    std::span<const value_type> argument_types() const noexcept { return _arguments; }
    /// Borrows the complete frozen result descriptor.
    /// Example: `auto extent = bound.result_type().byte_layout();`.
    const value_type& result_type() const noexcept { return _result; }
    /// Borrows the frozen program's canonical frame reservation requirements.
    /// Precondition: this handle has not been moved from.
    /// Example: `auto r = bound.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept;
    /// Executes trusted canonical inputs and transfers one owned result into caller storage.
    /// Safety preconditions: this binding is live; arguments have prepared arity;
    /// each emitted read's descriptor and live aligned canonical extent matches its bound type
    /// and ownership plan, outlives the call, and does not alias mutated stack/result bytes.
    /// Unused slots may contain source-unit placeholders; they are never materialized.
    /// No operation accesses a mutable alias of borrowed inputs during execution.
    /// Destination is uninitialized, aligned, disjoint storage of result_type().byte_layout();
    /// null is permitted for every zero-byte layout. The caller exclusively owns success.
    /// On an exception, destination remains uninitialized and the execution suffix is retired.
    /// Scalar dispatch allocates nothing on a sufficiently reserved caller stack.
    /// Complexity: O(argument slots + executed instructions + payload work).
    /// Example: `bound.call_into(stack, inputs, destination);`.
    void call_into(raw_stack& stack,
                   std::span<const borrowed_value> arguments,
                   std::byte* result_destination) const;
    /// Retires an initialized canonical result without freeing its caller-owned outer buffer.
    /// Safety preconditions: result is a live exclusively owned result from this binding;
    /// its aligned complete extent remains valid and has not been retired or byte-transferred.
    /// Never call this after failed execution. The binding outlives retirement.
    /// Complexity: O(visited ownership paths and array nodes); no allocation or throwing.
    /// Example: `bound.destroy_result(destination);`.
    void destroy_result(std::byte* result) const noexcept;
};

namespace detail {
/// Borrows canonical bytes from a public owner without exposing its private payload.
const std::byte* canonical_data(const value& source) noexcept;
/// Borrows a native leaf or owns a used composite's trusted canonical conversion.
template <class T>
struct typed_input {
    /// Selects borrowed native leaves or independently owned canonical composites.
    using storage = std::conditional_t<native_leaf<T>, std::reference_wrapper<const T>, value>;
    storage source; ///< Borrows a leaf or owns independent canonical composite input bytes.

    /// Binds a leaf address or writes only a used composite through its trusted adapter.
    /// Complexity: O(used composite payload size) time and owned space; leaves do not allocate.
    static storage bind(const T& input, bool used) {
        if constexpr (native_leaf<T>) {
            return std::cref(input);
        } else {
            return used ? value::from(input) : value{};
        }
    }
    /// Establishes one borrowed leaf or optional canonical composite owner.
    /// Complexity: O(used composite payload size) time and owned space.
    explicit typed_input(const T& input, bool used) : source(bind(input, used)) {}
    /// Borrows matching canonical bytes with a previously bound descriptor.
    borrowed_value borrow(const value_type& type) const noexcept {
        if constexpr (native_leaf<T>) {
            return {type, reinterpret_cast<const std::byte*>(&source.get())};
        } else {
            return {type, canonical_data(source)};
        }
    }
};
/// Owns aligned outer bytes and optionally one initialized canonical result.
/// This helper is not an operand relocation or erased move interface.
class result_storage {
    const prepared_raw_segment& _binding;
    std::byte* _data;
    bool _initialized = false;

public:
    /// Allocates the frozen result's aligned outer extent.
    /// Complexity: O(result byte extent) allocated space.
    explicit result_storage(const prepared_raw_segment& binding);
    /// Retires a successful result and frees outer storage on every exit.
    /// Complexity: O(result ownership paths).
    ~result_storage();
    /// Disallows duplicate ownership.
    result_storage(const result_storage&) = delete;
    /// Disallows assignment.
    result_storage& operator=(const result_storage&) = delete;
    /// Borrows the outer destination bytes.
    std::byte* data() const noexcept { return _data; }
    /// Records successful result initialization for exception-safe extraction.
    void initialized() noexcept { _initialized = true; }
};
} // namespace detail

/// Borrows typed inputs and returns independent owning results through bound adapters.
/// Leaves use direct addresses/result bytes; tuples/vectors use trusted canonical adapters.
/// Composite conversion/result ownership may allocate; no recursive checks occur during calls.
/// Handles share immutable engine state, not independent copies of mutable captures.
/// Example: `auto result = bound.call(stack, 2.0);`.
template <class R, class... Args>
class prepared_segment {
    prepared_raw_segment _binding;
    friend class compiled_segment;
    /// Transfers a complete construction-validated binding.
    explicit prepared_segment(prepared_raw_segment binding) : _binding(std::move(binding)) {}
    /// Converts only emitted composite reads, retaining unused inputs as empty source unit.
    /// Complexity: O(arity + read payload size) time and canonical input space.
    template <std::size_t... I>
    auto bind(const std::tuple<const Args&...>& inputs, std::index_sequence<I...>) const {
        return std::tuple<detail::typed_input<Args>...>{
            detail::typed_input<Args>(std::get<I>(inputs), _binding._used[I])...};
    }
    /// Binds source-order borrowed pointer slots without signature traversal.
    /// Complexity: O(arity) time and stack space.
    template <std::size_t... I>
    auto borrow(const std::tuple<detail::typed_input<Args>...>& inputs,
                std::index_sequence<I...>) const {
        return std::array<borrowed_value, sizeof...(Args)>{
            (_binding._used[I] ? std::get<I>(inputs).borrow(_binding.argument_types()[I]) :
                                 borrowed_value{_binding._unit, nullptr})...};
    }

public:
    /// Disallows independent capture copies.
    prepared_segment(const prepared_segment&) = delete;
    /// Disallows copied assignment.
    prepared_segment& operator=(const prepared_segment&) = delete;
    /// Transfers typed binding ownership.
    prepared_segment(prepared_segment&&) noexcept = default;
    /// Transfers typed binding ownership.
    /// Complexity: O(old binding metadata and last-owned captures).
    prepared_segment& operator=(prepared_segment&&) noexcept = default;
    /// Borrows the frozen program's canonical frame reservation requirements.
    /// Precondition: this handle has not been moved from.
    /// Composite conversions and arbitrary callback/nested costs are not inferred.
    /// Example: `auto r = bound.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept { return _binding.requirements(); }
    /// Executes with borrowed const inputs and returns a disjoint result.
    /// Precondition: the handle is live, and inputs have no mutable alias accessed by callbacks.
    /// Exceptions propagate after suffix cleanup; inputs and caller prefix remain owned.
    /// Scalar calls allocate nothing on a warmed sufficiently reserved stack.
    /// Complexity: O(arity + instructions + payload work) time and independent result space.
    /// Example: `bound.call(stack, 2.0)`.
    R call(raw_stack& stack, const Args&... arguments) const {
        const auto inputs = bind(std::tie(arguments...), std::index_sequence_for<Args...>{});
        const auto views = borrow(inputs, std::index_sequence_for<Args...>{});
        if constexpr (detail::native_leaf<R>) {
            alignas(R) std::array<std::byte, sizeof(R)> bytes{};
            _binding.call_into(stack, views, bytes.data());
            return detail::concrete_adapter<R>::read(_binding.result_type(), bytes.data());
        } else {
            detail::result_storage result(_binding);
            _binding.call_into(stack, views, result.data());
            result.initialized();
            return detail::concrete_adapter<R>::read(_binding.result_type(), result.data());
        }
    }
};

/// Constructs exact adapter witnesses once before creating a typed binding.
template <class R, class... Args>
prepared_segment<R, Args...> compiled_segment::prepare() const {
    static_assert(!std::is_void_v<R>, "use tuple<> for the source-unit result");
    const std::array<value_type, sizeof...(Args)> types{detail::concrete_adapter<Args>::type()...};
    return prepared_segment<R, Args...>(prepare_raw(types, detail::concrete_adapter<R>::type()));
}
} // namespace cel
#endif
