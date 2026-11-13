// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_DYN_SEGMENT_HPP
#define CEL_RUNTIME_DYN_SEGMENT_HPP
#include <cel/runtime/detail/raw_segment.hpp>
#include <concepts>
#include <new>
#include <optional>
#include <vector>

namespace cel {
/// \addtogroup runtime_programs
/// @{

/// A byte cursor and live-slot depth below a fragment's local operands.
///
/// Both coordinates must match; zero-byte prefixes still contribute live slots.
/// Defaults to zero bytes and zero slots, the origin admitted for independent execution.
///
/// Example: `dyn_segment rhs(parent.before_top());`.
struct stack_origin {
    std::size_t bytes = 0; ///< Gives the ambient canonical cursor.
    std::size_t depth = 0; ///< Gives the ambient live-slot count.
};
/// Compares both ambient coordinates exactly.
///
/// Example: `stack_origin{} == stack_origin{0, 0}`.
inline bool operator==(const stack_origin& left, const stack_origin& right) noexcept {
    return left.bytes == right.bytes && left.depth == right.depth;
}
class compiled_segment;
namespace detail {
class prepared_raw_segment;
}
template <class R, class... Args>
class prepared_segment;

/// Total byte and slot capacities for one prefixed execution.
///
/// Both fields include the live caller prefix; neither counts separately owned composite payloads.
///
/// Example: `stack.reserve(r.bytes.size, r.bytes.alignment); stack.reserve_depth(r.slots);`.
struct stack_reservation {
    layout bytes;      ///< Gives total byte capacity and required alignment.
    std::size_t slots; ///< Gives total live-slot capacity, including the caller prefix.
};

/// The statically known storage requirements of a frozen ordinary program.
///
/// Extent and slots are peak simultaneous requirements, not cumulative instruction costs.
/// Defaults to alignment one with no frame bytes or live slots.
/// Composite payload allocations, concrete callback temporaries, and arbitrary callback or
/// nested external stack work are not inferred. Result destinations remain caller-owned.
///
/// Example: `auto r = program.requirements().for_prefix(stack.cursor(), stack.depth());`.
struct stack_requirements {
    std::size_t alignment = 1;    ///< Gives maximum intermediate canonical alignment.
    std::size_t frame_extent = 0; ///< Gives peak canonical extent for a zero-cursor frame.
    std::size_t live_slots = 0;   ///< Gives peak simultaneously live frame operands.

    /// Returns checked conservative total capacities for an existing caller prefix.
    ///
    /// Includes alignment padding and the live byte/depth prefix.
    /// Reserve before entering a frame, using cursor/depth measured without intervening mutation.
    /// raw_stack::reserve retains any higher existing alignment.
    /// Does not allocate or traverse type metadata.
    ///
    /// Throws std::invalid_argument for invalid alignment and std::overflow_error if either
    /// total capacity is unrepresentable.
    ///
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
/// Returns an exact const call-operator specialization, or void if none is supported.
///
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
/// Returns whether a const callback has the exact operand and result identities.
///
/// Compares identities after cv/reference removal; generic signatures must be deduction-proven.
template <class F, class R, class... A>
consteval bool exact_callback() {
    using callback_type = std::decay_t<F>;
    if constexpr (!std::is_invocable_v<const callback_type&, A...>)
        return false;
    else {
        constexpr bool result_matches =
            std::same_as<R, std::remove_cvref_t<std::invoke_result_t<const callback_type&, A...>>>;
        if constexpr (requires { typename callback_signature<callback_type>::arguments; })
            return result_matches &&
                   std::same_as<typename callback_signature<callback_type>::arguments,
                                std::tuple<A...>>;
        else if constexpr (requires {
                               typename callback_signature<
                                   decltype(&callback_type::operator())>::arguments;
                           })
            return result_matches &&
                   std::same_as<
                       typename callback_signature<decltype(&callback_type::operator())>::arguments,
                       std::tuple<A...>>;
        else {
            using member =
                decltype(generic_member<callback_type,
                                        std::invoke_result_t<const callback_type&, A...>, A...>());
            if constexpr (std::is_void_v<member>)
                return false;
            else
                return result_matches && std::is_invocable_v<member, const callback_type&, A...>;
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
/// An owned callback with an exact, unambiguous const invocation signature.
template <class R, class... A, class F>
class signature_callback<R(A...), F> {
    F _callback;

public:
    /// Constructs the owned callback from its source.
    ///
    /// Complexity: the callback's construction time and owned storage.
    template <class G>
    requires(!std::same_as<std::remove_cvref_t<G>, signature_callback> &&
             std::constructible_from<F, G&&>) explicit signature_callback(G&& callback) :
        _callback(std::forward<G>(callback)) {}
    /// Invokes exactly the previously selected const member.
    ///
    /// Complexity: the callback's invocation costs.
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
/// The validated cursor and storage requirements of one pending operation.
struct construction_effect {
    std::size_t cursor;              ///< Gives the resulting zero-origin frame cursor.
    stack_requirements requirements; ///< Gives the staged peak requirements.
};
} // namespace detail

/// Selects an exact const callback member from an ambiguous or uninspectable overload set.
///
/// Signature includes actual parameter and return reference qualifiers; no conversion
/// creates a signature that the callback does not declare. The wrapper owns F normally.
/// Generic callbacks that cannot prove their instantiated parameter identities use this
/// explicit selection. Function pointers can instead be supplied directly with an exact cast.
///
/// Complexity: the callback's construction time and owned capture storage.
///
/// Example: `builder.op1<double, double>(with_signature<double(double)>(overloaded{}));`.
template <class Signature, class F>
requires(detail::signature_binding<Signature, std::decay_t<F>>::available) auto with_signature(
    F&& callback) {
    return detail::signature_callback<Signature, std::decay_t<F>>(std::forward<F>(callback));
}

/// A move-only builder of checked, source-ordered operations.
///
/// Operations consume older-first exact operands and report runtime failures with exceptions.
/// Successful finish freezes captures and descriptors independently of builder lifetime.
/// Owned callbacks retain their captures; referenced external state stays caller-owned.
/// Destruction releases unfinished captures and metadata.
///
/// Complexity: destruction is linear in released code, metadata, and client destructor work.
/// Emission costs include amortized O(1) instruction bookkeeping and, on logical stack growth,
/// O(current stack depth) metadata transfer in addition to each operation's stated costs.
///
/// Example: `dyn_segment b; b.just(value::from(2.0)); auto p = std::move(b).finish();`.
/// See the runtime.hpp example for complete construction, preparation, reservation, and execution.
class dyn_segment {
    detail::raw_segment _body;
    std::vector<value_type> _stack;
    std::vector<detail::argument_read> _reads;
    std::vector<std::size_t> _end_cursors; // Mirrors canonical cursor ends in _stack order.
    stack_requirements _requirements;      // Retains committed intermediate storage peaks.
    stack_origin _origin; // Retains the ambient origin independently of local operands.
    std::vector<std::optional<stack_requirements>> _known_calls; // Tracks direct frozen callees.
    /// Emits a consuming fixed-aggregate operation with the specified field permutation.
    ///
    /// Throws std::invalid_argument for wrong origin, shape, or permutation before publication.
    ///
    /// Complexity: O(field count + complete type-comparison work) construction time and
    /// O(field count) retained staging metadata, plus the class's emission costs.
    void pack(value_type type, std::span<const std::size_t> permutation, stack_origin ambient);
    /// Emits a consuming projection of the specified tuple or record field.
    ///
    /// Throws std::invalid_argument for the wrong category, missing operand, or invalid index.
    ///
    /// Complexity: complete type-comparison work during construction; sibling retirement and
    /// selected canonical byte transfer during execution, plus the class's emission costs.
    void project(std::size_t index, type_kind category);
    /// Validates an operation's ordered operands and returns its pending storage effect.
    ///
    /// \pre outputs contains at most one result.
    ///
    /// Throws std::invalid_argument for underflow, incompatible operands, or absent ownership.
    /// Throws std::overflow_error for canonical frame extent or depth overflow.
    ///
    /// Complexity: O(arity + complete type-comparison work) time and comparison auxiliary space,
    /// plus O(current stack depth) metadata transfer on capacity growth.
    detail::construction_effect check_operation(std::span<const value_type> inputs,
                                                std::span<const value_type> outputs);
    /// Publishes a previously validated logical stack effect without allocation.
    ///
    /// \pre check_operation supplied effect and body publication succeeded;
    /// outputs contains at most one result.
    ///
    /// Complexity: O(consumed operand count).
    void apply_operation(std::size_t consumed,
                         std::span<const value_type> outputs,
                         const detail::construction_effect& effect) noexcept;

public:
    /// Creates an empty independent builder.
    ///
    /// Example: `dyn_segment builder;`.
    dyn_segment() = default;
    /// Creates an empty dependent fragment at the supplied ambient origin.
    ///
    /// A nonzero byte or depth origin prevents independent execution or preparation after finish.
    /// Subsequent emission throws std::overflow_error for an unrepresentable absolute
    /// cursor or depth.
    ///
    /// Example: `dyn_segment branch(parent.before_top());`.
    explicit dyn_segment(stack_origin origin) : _origin(origin) {}
    dyn_segment(const dyn_segment&) = delete;
    dyn_segment& operator=(const dyn_segment&) = delete;
    /// Transfers construction ownership.
    ///
    /// Use the source only for destruction or assignment after moving.
    dyn_segment(dyn_segment&&) noexcept = default;
    /// Transfers construction ownership, releasing prior captures and metadata.
    ///
    /// Use the source only for destruction or assignment after moving, including self-move.
    /// Complexity: O(old captures and metadata).
    dyn_segment& operator=(dyn_segment&&) noexcept = default;
    /// Returns the retained ambient origin.
    ///
    /// Example: `builder.origin() == stack_origin{}`.
    stack_origin origin() const noexcept { return _origin; }
    /// Borrows the requested local logical operand suffix in older-first order.
    ///
    /// Returns an empty view for zero arity or underflow; ambient origins are not operands.
    /// Leaves construction and callback state unchanged without allocating or finishing code.
    /// Do not use the view after emission, move, assignment, or destruction of this builder.
    /// Failed emission can also invalidate the view.
    /// Copying a descriptor from the view retains its immutable metadata independently.
    ///
    /// Example: `auto inputs = builder.operand_types(2); if (inputs.size() != 2) return;`.
    std::span<const value_type> operand_types(std::size_t arity) const noexcept {
        if (arity > _stack.size()) return {};
        return std::span<const value_type>(_stack).last(arity);
    }
    /// Returns the current absolute logical cursor and live-slot depth.
    ///
    /// Throws std::overflow_error if the absolute depth is unrepresentable.
    ///
    /// Example: `auto ambient = builder.snapshot();`.
    stack_origin snapshot() const;
    /// Returns the absolute origin below count local operands.
    ///
    /// Zero returns the current cursor/depth; the full local depth returns origin().
    ///
    /// Throws std::invalid_argument on local underflow and std::overflow_error on depth overflow.
    ///
    /// Example: `builder.make_tuple(2, builder.origin_below(2));`.
    stack_origin origin_below(std::size_t count) const;
    /// Returns the origin immediately below the top local operand.
    ///
    /// Throws std::invalid_argument if no local operand exists, or std::overflow_error
    /// if the absolute depth is unrepresentable.
    ///
    /// Example: `dyn_segment then_body(builder.before_top());`.
    stack_origin before_top() const;
    /// Emits an operation that consumes count source-ordered operands and returns a tuple.
    ///
    /// Count zero emits source unit. Execution transfers children without cloning them.
    ///
    /// Throws std::invalid_argument for missing operands or wrong byte/depth origin before
    /// publication, or std::overflow_error for canonical layout/accounting overflow.
    ///
    /// Complexity: O(count + complete type-comparison work) construction time and O(count)
    /// retained metadata; O(count + outer tuple byte size) execution work.
    ///
    /// Example: `builder.make_tuple(2, stack_origin{});`.
    void make_tuple(std::size_t count, stack_origin ambient);
    /// Emits an operation that consumes count exact elements and returns an owning array.
    ///
    /// Requires element ownership capability even when count is zero; execution transfers children.
    ///
    /// Throws std::invalid_argument for missing operands, incompatible elements, absent ownership,
    /// or wrong ambient origin; std::overflow_error for extent/accounting overflow.
    ///
    /// Complexity: O(count + complete type-comparison work) construction time and O(count)
    /// retained metadata; O(count + total canonical element bytes) execution work and array
    /// storage.
    ///
    /// Example: `builder.make_array(0, value_type::leaf<double>(), {});`.
    void make_array(std::size_t count, value_type element, stack_origin ambient);
    /// Emits a consuming record construction from schema-ordered operands.
    ///
    /// Follows the permutation overload's ownership, validation, and complexity contracts.
    ///
    /// Example: `builder.make_record(schema, {});`.
    void make_record(value_type record, stack_origin ambient);
    /// Emits a consuming record construction from source-ordered fields and a schema permutation.
    ///
    /// Each permutation[i] gives the schema position of source operand i, one entry per field.
    /// The permutation does not change expression evaluation order; children transfer without
    /// clones. Copies the map and retains record; neither input must outlive construction.
    ///
    /// Throws std::invalid_argument for a non-record, wrong origin/types, or an incomplete or
    /// nonbijective map before publication; std::overflow_error for extent/accounting overflow.
    ///
    /// Complexity: O(field count + complete type-comparison work) construction time and
    /// O(field count) retained metadata; O(field count + outer record byte size) execution work.
    ///
    /// Example: `builder.make_record(schema, std::array<std::size_t, 2>{1, 0}, {});`.
    void make_record(value_type record,
                     std::span<const std::size_t> permutation,
                     stack_origin ambient);
    /// Emits an operation that consumes a tuple and transfers the selected field without cloning.
    ///
    /// Execution retires the tuple's other fields.
    ///
    /// Throws std::invalid_argument for a missing or non-tuple operand or invalid index
    /// before publication.
    ///
    /// Complexity: complete type-comparison work during construction; sibling retirement and
    /// selected canonical byte transfer during execution.
    ///
    /// Example: `builder.project_tuple(0);`.
    void project_tuple(std::size_t index);
    /// Emits an operation that consumes a record and transfers the named field without cloning.
    ///
    /// Execution retires the record's other fields.
    ///
    /// Throws std::invalid_argument for a missing or non-record operand or missing field
    /// before publication.
    ///
    /// Complexity: field-name search and complete type-comparison work during construction;
    /// sibling retirement and selected canonical byte transfer during execution. Name search
    /// is linear in field count and the bytes compared against name.
    ///
    /// Example: `builder.project_record("enabled");`.
    void project_record(std::string_view name);
    /// Emits an operation that consumes an array and f64 index and transfers the selected element.
    ///
    /// Execution retires the other elements without cloning the selected element.
    ///
    /// Throws std::invalid_argument for missing or incompatible operands before publication.
    /// Execution throws std::out_of_range for nonfinite, negative, fractional, unrepresentable,
    /// or out-of-bounds indices.
    ///
    /// Complexity: complete type-comparison work during construction; sibling retirement and
    /// selected canonical byte transfer during execution.
    ///
    /// Example: `builder.just(value::from(0.0)); builder.index_array();`.
    // [knowledge] Check representability before the size_t cast, then exact bounds before access.
    void index_array();
    /// Emits a range construction consuming its source-ordered f64 endpoints.
    ///
    /// Full ranges consume no endpoints.
    /// No floating iteration semantics are supplied.
    ///
    /// Throws std::invalid_argument for an invalid kind or wrong operands, or std::overflow_error
    /// for canonical accounting overflow.
    ///
    /// Complexity: endpoint type-comparison work during construction; constant execution work.
    ///
    /// Example: `builder.make_range(range_kind::range_full);`.
    void make_range(range_kind kind);
    /// Emits a conditional that consumes bool and executes exactly one dependent body.
    ///
    /// Bodies originate immediately below the condition and produce ownership-compatible results.
    /// All reads in both bodies are registered and preflighted before parent execution effects.
    /// Reservations use the maximum of both bodies' demands.
    ///
    /// Throws std::invalid_argument for a missing or non-bool condition, wrong origins, bodies
    /// without exactly one result, or incompatible results/reads before publication.
    /// Throws std::overflow_error for accounting overflow.
    ///
    /// Complexity: O(P + B * (P + B) + complete type-comparison work) construction time and
    /// O(P + B) retained read storage for P parent reads and B combined body reads.
    /// Execution costs only the selected body's work.
    ///
    /// Example: `builder.if_else(std::move(yes), std::move(no));`.
    void if_else(dyn_segment then_body, dyn_segment else_body);
    /// Emits a conjunction that consumes bool and executes bool rhs only when the left is true.
    ///
    /// Follows if_else's origins, shared-read preflight, exceptions, and maximum-demand contracts.
    /// Complexity: follows if_else for parent/rhs read merging; O(selected rhs work) execution.
    ///
    /// Example: `builder.logical_and(std::move(rhs));`.
    void logical_and(dyn_segment rhs);
    /// Emits a disjunction that consumes bool and executes bool rhs only when the left is false.
    ///
    /// Follows if_else's origins, shared-read preflight, exceptions, and maximum-demand contracts.
    /// Complexity: follows if_else for parent/rhs read merging; O(selected rhs work) execution.
    ///
    /// Example: `builder.logical_or(std::move(rhs));`.
    void logical_or(dyn_segment rhs);
    /// Emits an installed callable retaining explicit parameters and independent frozen code.
    ///
    /// Results admit all complete owning types. Execution clones a reference-semantic carrier,
    /// not the callable's captures.
    ///
    /// \pre body has not been moved from.
    ///
    /// Throws std::invalid_argument for a dependent body, invalid parameters, or undeclared or
    /// incompatible reads; std::overflow_error for canonical accounting overflow.
    ///
    /// Complexity: parameter binding and complete type-comparison work during construction;
    /// constant-size carrier allocation and clone during execution.
    ///
    /// Example: `builder.push_closure(std::move(body).finish(), parameters);`.
    void push_closure(compiled_segment body, std::span<const value_type> parameters);
    /// Emits an invocation consuming an installed callee and its source-ordered arguments.
    ///
    /// Complete ownership is bound during construction. Arguments stay valid across nested stack
    /// growth. Same-carrier exclusion covers staging, body execution, result publication, and
    /// cleanup. Known direct callee requirements are included; signature-only external costs are
    /// not inferred. Returns one independent result owner; callable components share code.
    ///
    /// Throws std::invalid_argument for missing operands or arity/signature/carrier mismatch
    /// before publication; std::overflow_error for canonical accounting overflow.
    /// Execution exceptions retire acquired owners and preserve the caller prefix and original
    /// exception. Cleanup does not undo external callback effects.
    ///
    /// Complexity: O(arity + complete type-comparison work) construction time and retained
    /// signature storage; argument canonical-byte transfer and callee work during execution.
    /// Execution allocates argument pointer tables and outer argument/result storage, not
    /// scalar public owner boxes.
    ///
    /// Example: `builder.invoke_closure(2);`.
    void invoke_closure(std::size_t arity);
    /// Emits a read that clones a borrowed positional input when the program executes.
    ///
    /// Repeated declarations must have identical complete shapes and ownership plans.
    ///
    /// Throws std::invalid_argument for incompatible repeated declarations or missing clone
    /// capability. Throws std::overflow_error for canonical frame extent or depth overflow.
    ///
    /// Complexity: O(emitted read count + complete type-comparison work) construction time;
    /// cloning the input's canonical payload during execution. Class emission costs also apply.
    ///
    /// Example: `builder.push_argument(0, value_type::leaf<double>());`.
    void push_argument(std::size_t index, value_type type);
    /// Retains a literal owner and emits an independent clone on every execution.
    ///
    /// Callable components share code under value's copy semantics rather than copying captures.
    ///
    /// Throws std::invalid_argument if the complete value cannot be cloned.
    /// Throws std::overflow_error for canonical frame extent or depth overflow.
    ///
    /// Complexity: ownership admission and class emission costs during construction, retaining
    /// one owner; the literal's canonical clone costs during execution.
    ///
    /// Example: `builder.just(value::from(true));`.
    void just(value literal);
    /// Emits an arbitrary-arity const callback with exact declared operands and result.
    ///
    /// R is a trusted concrete adapter type or void; void consumes without producing a value.
    /// Parameter/result identities are exact after cv/reference removal. Reference results are
    /// copied into an independently owned result; implicit result conversions are not admitted.
    /// Use with_signature for ambiguous or uninspectable callback members. Generic members
    /// require deduction-proven exact parameter identities.
    /// Lvalue callbacks are copied; rvalue callbacks are moved into owned decayed storage.
    /// Referenced objects remain caller-owned, must outlive invocation, and require their own
    /// synchronization. Captures require nonthrowing destruction.
    /// Execution consumes the declared operands, invokes the callback, then publishes its result.
    /// Over-aligned callback operands use reference parameters on MSVC.
    ///
    /// Throws std::invalid_argument for current stack mismatch before publication.
    /// Throws std::overflow_error for canonical frame extent or depth overflow.
    /// Callback exceptions propagate through the execution interface after suffix cleanup.
    ///
    /// Complexity: O(arity + adapter construction + complete type-comparison work + callback
    /// construction) emission time, plus the class's bookkeeping costs. Execution includes
    /// concrete operand/result adaptation and callback work.
    ///
    /// Example: `builder.op<double, double>([](double n) { return n * 2.0; });`.
    template <class R, class... Args, class F>
    requires(detail::exact_callback<F, R, Args...>()) void op(F&& callback) {
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
            [callback = std::forward<F>(callback)](detail::frame&, Args&&... operands) -> R {
                return std::invoke(callback, std::move(operands)...);
            });
        apply_operation(sizeof...(Args), outputs, effect);
    }
    /// Emits a zero-operand callback; void produces no value, otherwise pushes one result.
    ///
    /// Follows op's callback, validation, failure, and complexity contracts.
    ///
    /// Example: `builder.op0<double>([] { return 2.0; });`.
    template <class R, class F>
    requires(detail::exact_callback<F, R>()) void op0(F&& callback) {
        op<R>(std::forward<F>(callback));
    }
    /// Emits a unary operation that consumes one exact operand and produces R unless it is void.
    ///
    /// Follows op's callback, validation, failure, and complexity contracts.
    ///
    /// Example: `builder.op1<bool, bool>([](bool b) { return !b; });`.
    template <class R, class A, class F>
    requires(detail::exact_callback<F, R, A>()) void op1(F&& callback) {
        op<R, A>(std::forward<F>(callback));
    }
    /// Emits a binary operation that consumes older-first operands and produces R unless it is
    /// void.
    ///
    /// Follows op's callback, validation, failure, and complexity contracts.
    ///
    /// Example: `builder.op2<double, double, double>([](double a, double b) { return a - b; });`.
    template <class R, class A, class B, class F>
    requires(detail::exact_callback<F, R, A, B>()) void op2(F&& callback) {
        op<R, A, B>(std::forward<F>(callback));
    }
    /// Emits a ternary operation consuming source-ordered operands and producing R unless it is
    /// void.
    ///
    /// Follows op's callback, validation, failure, and complexity contracts.
    ///
    /// Example: `builder.op3<double, double, double, double>(
    /// [](double a, double b, double c) { return a + b + c; });`.
    template <class R, class A, class B, class C, class F>
    requires(detail::exact_callback<F, R, A, B, C>()) void op3(F&& callback) {
        op<R, A, B, C>(std::forward<F>(callback));
    }
    /// Freezes exactly one result, mapping an empty final stack to source unit.
    ///
    /// Retains the ambient origin; finishing does not make a dependent fragment independent.
    ///
    /// Throws std::invalid_argument for leftover operands instead of discarding them.
    ///
    /// Complexity: constant bookkeeping and frozen-program allocation; an empty stack also
    /// incurs source-unit emission costs.
    ///
    /// Example: `auto program = std::move(builder).finish();`.
    compiled_segment finish() &&;
};

/// A move-only handle to immutable frozen code and its captures.
///
/// Prepared bindings and callables retain the code after this handle is destroyed.
/// Captures are never copied as independent values; external referenced state stays caller-owned.
/// Moves between distinct handles leave the source without code; assign a live handle before
/// using borrowed accessors.
/// Borrowed result types and requirements remain valid while unchanged code ownership
/// retains them. Execution requires exclusive access to its stack and any mutable callback state.
/// Last ownership retires managed captures and nested executable bodies without allocation or
/// nesting-dependent stack. Arbitrary client capture destructors retain their own costs.
///
/// Complexity: last release takes O(released code nodes/edges + metadata + client destructor work).
///
/// Example: `auto prepared = program.prepare<double, double>();`.
class compiled_segment {
    detail::retirement_ptr<const detail::frozen_program> _program;
    friend class dyn_segment;
    friend struct detail::segment_access;
    /// Retains a fully checked frozen engine.
    explicit compiled_segment(
        detail::retirement_ptr<const detail::frozen_program> program) noexcept;

    /// Binds complete runtime shapes and ownership plans once.
    ///
    /// Retains copied argument/result descriptors and frozen code, not the supplied spans.
    /// Checks only emitted argument positions; unused descriptors need not support ownership.
    ///
    /// Throws std::invalid_argument for dependent origin, undeclared reads, incompatible types,
    /// or absent ownership.
    /// Throws std::logic_error for a moved-from handle.
    ///
    /// Complexity: O(argument count + complete type-comparison work) time and argument binding
    /// storage, plus comparison auxiliary space.
    detail::prepared_raw_segment prepare_raw(std::span<const value_type> arguments,
                                             const value_type& result) const;

public:
    compiled_segment(const compiled_segment&) = delete;
    compiled_segment& operator=(const compiled_segment&) = delete;
    /// Transfers the frozen handle.
    compiled_segment(compiled_segment&&) noexcept = default;
    /// Transfers the handle and releases prior code when uniquely owned.
    ///
    /// Self-move leaves this handle unchanged.
    /// Complexity: O(prior captures and metadata on last release).
    compiled_segment& operator=(compiled_segment&&) noexcept = default;
    /// Returns the complete frozen result descriptor.
    ///
    /// The reference borrows retained code; copying the descriptor retains its metadata.
    ///
    /// \pre this handle has not been moved from.
    ///
    /// Example: `auto type = program.result_type();`.
    const value_type& result_type() const noexcept;
    /// Borrows frozen intermediate alignment, peak canonical extent, and live-slot depth.
    ///
    /// Arbitrary callback/nested costs are not inferred; use for_prefix before reserving.
    ///
    /// \pre this handle has not been moved from.
    ///
    /// Example: `auto r = program.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept;
    /// Executes checked positional inputs and returns one independent result owner.
    ///
    /// Validates every emitted read before effects.
    /// Extra unused inputs are permitted and are not materialized.
    /// Borrows arguments for the call.
    /// Preserves the caller's stack prefix, not external callback effects.
    /// Returned callable components retain reference-semantic code.
    ///
    /// \pre callbacks do not mutate the borrowed arguments or their owned payloads during the call.
    ///
    /// Throws std::invalid_argument for dependent origin or missing/incompatible read
    /// types/ownership. Throws std::logic_error for a moved-from handle or canonical-phase stack
    /// reentry. Callback/acquisition exceptions propagate after nonallocating suffix cleanup.
    ///
    /// Complexity: O(read validation + executed instructions + payload work) time and result space.
    ///
    /// Example: `auto result = program.execute(stack, inputs);`.
    value execute(raw_stack& stack, std::span<const value> arguments) const;
    /// Binds exact recursive concrete adapters once, admitting valid client leaves at roots.
    ///
    /// R must not be void; use std::tuple<> for a source-unit result.
    /// Unused declared arguments are accepted and are not converted during calls.
    ///
    /// Throws std::invalid_argument for incompatible argument/result shapes, undeclared reads,
    /// or a dependent origin; std::logic_error for a moved-from handle.
    ///
    /// Complexity: O(arity + adapter construction + complete type-comparison work) time and
    /// argument binding storage, plus adapter result storage and comparison auxiliary space.
    ///
    /// Example: `auto bound = program.prepare<double, double>();`.
    template <class R, class... Args>
    prepared_segment<R, Args...> prepare() const;
};

namespace detail {
/// A move-only binding to frozen code and a complete canonical signature.
///
/// No call compares descriptors, converts operands, or allocates scalar public owner boxes.
/// Referenced callback state remains caller-owned and supplies its own synchronization.
/// Borrowed descriptor views remain valid until this binding is moved, assigned, or destroyed.
/// After moving, use the source only for destruction or assignment, including after self-move.
/// Destruction releases retained code and metadata.
///
/// Complexity: last release takes O(released code nodes/edges + metadata + client destructor work).
class prepared_raw_segment {
    detail::retirement_ptr<const detail::frozen_program> _program;
    std::vector<value_type> _arguments;
    value_type _result;
    std::vector<bool> _used;
    value_type _unit;
    friend class cel::compiled_segment;
    friend struct segment_access;
    template <class R, class... Args>
    friend class cel::prepared_segment;
    /// Retains a validated frozen signature.
    ///
    /// Copies the argument descriptors; their source span need not outlive construction.
    ///
    /// Complexity: O(argument count) time and binding space.
    prepared_raw_segment(detail::retirement_ptr<const detail::frozen_program> program,
                         std::span<const value_type> arguments,
                         value_type result);

public:
    prepared_raw_segment(const prepared_raw_segment&) = delete;
    prepared_raw_segment& operator=(const prepared_raw_segment&) = delete;
    /// Transfers the frozen binding.
    prepared_raw_segment(prepared_raw_segment&&) noexcept = default;
    /// Transfers the binding and releases old metadata/code.
    ///
    /// Complexity: O(old argument metadata and last-owned captures).
    prepared_raw_segment& operator=(prepared_raw_segment&&) noexcept = default;
    /// Borrows prepared source-order argument descriptors.
    ///
    /// Includes unused declarations; the returned span does not retain the binding.
    ///
    /// Example: `auto arity = bound.argument_types().size();`.
    std::span<const value_type> argument_types() const noexcept { return _arguments; }
    /// Borrows the complete frozen result descriptor.
    ///
    /// Copy the descriptor to retain its metadata independently of the binding.
    ///
    /// Example: `auto extent = bound.result_type().byte_layout();`.
    const value_type& result_type() const noexcept { return _result; }
    /// Borrows the frozen program's canonical frame reservation requirements.
    ///
    /// \pre this handle has not been moved from.
    ///
    /// Example: `auto r = bound.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept;
    /// Executes trusted canonical inputs and transfers one owned result into caller storage.
    ///
    /// Unused slots may contain source-unit placeholders; they are never materialized.
    /// The caller exclusively owns the successful result.
    /// Scalar dispatch allocates nothing on a sufficiently reserved caller stack.
    ///
    /// \pre each emitted read's descriptor and live aligned canonical extent matches its bound type
    /// and ownership plan, outlives the call, and does not alias mutated stack/result bytes.
    /// No operation accesses a mutable alias of borrowed inputs.
    /// \pre destination is uninitialized, aligned, disjoint storage of result_type().byte_layout();
    /// null is permitted for every zero-byte layout.
    ///
    /// Throws std::logic_error for a moved-from binding or stack access during a canonical phase.
    /// Throws std::invalid_argument for the wrong prepared arity.
    /// On an exception, destination remains uninitialized and the execution suffix is retired.
    /// Callback exceptions propagate unchanged. Cleanup preserves the caller prefix, not
    /// external callback effects.
    ///
    /// Complexity: O(argument slots + executed instructions + payload work).
    ///
    /// Example: `bound.call_into(stack, inputs, destination);`.
    void call_into(raw_stack& stack,
                   std::span<const borrowed_value> arguments,
                   std::byte* result_destination) const;
    /// Retires an initialized canonical result without freeing its caller-owned outer buffer.
    ///
    /// Leaves the buffer uninitialized and caller-owned without allocation.
    ///
    /// \pre result is a live exclusively owned result from this binding;
    /// its aligned complete extent remains valid and has not been retired or byte-transferred.
    /// The binding outlives retirement. Do not call after failed execution or retire a result
    /// twice.
    ///
    /// Complexity: canonical ownership traversal and last-owned code/metadata retirement costs.
    ///
    /// Example: `bound.destroy_result(destination);`.
    void destroy_result(std::byte* result) const noexcept;
};

/// Borrows canonical bytes from a public owner without exposing its private payload.
///
/// The source owner must remain unchanged and alive while the bytes are borrowed.
const std::byte* canonical_data(const value& source) noexcept;
/// A borrowed native leaf or independently owned canonical composite input.
template <class T>
struct typed_input {
    /// Selects borrowed native leaves or independently owned canonical composites.
    using storage = std::conditional_t<native_leaf<T>, std::reference_wrapper<const T>, value>;
    storage source; ///< Borrows a leaf or owns independent canonical composite input bytes.

    /// Borrows a leaf, copies a used composite, or returns source unit for an unused composite.
    ///
    /// Complexity: O(used composite payload size) time and owned space; leaves do not allocate.
    static storage bind(const T& input, bool used) {
        if constexpr (native_leaf<T>) {
            return std::cref(input);
        } else {
            return used ? value::from(input) : value{};
        }
    }
    /// Establishes one borrowed leaf or optional canonical composite owner.
    ///
    /// Complexity: O(used composite payload size) time and owned space.
    explicit typed_input(const T& input, bool used) : source(bind(input, used)) {}
    /// Borrows matching canonical bytes with a previously bound descriptor.
    ///
    /// \pre type matches the concrete input; this input and any borrowed leaf outlive the view.
    borrowed_value borrow(const value_type& type) const noexcept {
        if constexpr (native_leaf<T>) {
            return {type, reinterpret_cast<const std::byte*>(&source.get())};
        } else {
            return {type, canonical_data(source)};
        }
    }
};
/// An aligned result buffer that retires an initialized result on destruction.
///
/// Borrows its binding, which must outlive the buffer.
class result_storage {
    const prepared_raw_segment& _binding;
    std::byte* _data;
    bool _initialized = false;

public:
    /// Allocates the frozen result's aligned outer extent.
    ///
    /// \pre binding is live and outlives this buffer.
    ///
    /// Complexity: O(result byte extent) allocated space.
    explicit result_storage(const prepared_raw_segment& binding);
    /// Retires a successful result and frees outer storage on every exit.
    ///
    /// Complexity: canonical result ownership and last-owned code/metadata retirement costs.
    ~result_storage();
    result_storage(const result_storage&) = delete;
    result_storage& operator=(const result_storage&) = delete;
    /// Borrows the outer destination bytes.
    std::byte* data() const noexcept { return _data; }
    /// Records successful result initialization for exception-safe extraction.
    ///
    /// \pre data() contains exactly one complete live result of the bound type.
    void initialized() noexcept { _initialized = true; }
};
} // namespace detail

/// A move-only typed binding that borrows inputs and returns concrete results.
///
/// Leaves use direct addresses/result bytes; tuples/vectors use trusted canonical adapters.
/// Composite conversion/result ownership may allocate; no recursive checks occur during calls.
/// Handles share immutable engine state, not independent copies of mutable captures.
/// Result vectors/tuples own independent mutable payloads; opaque leaves do not own pointees.
/// After moving, use the source only for destruction or assignment, including after self-move.
/// Destruction releases the binding and code.
///
/// Complexity: last release takes O(released code nodes/edges + metadata + client destructor work).
///
/// Example: `auto result = bound.call(stack, 2.0);`.
/// See the runtime.hpp example for a complete typed call and stack reservation.
template <class R, class... Args>
class prepared_segment {
    detail::prepared_raw_segment _binding;
    friend class compiled_segment;
    /// Transfers a complete construction-validated binding.
    explicit prepared_segment(detail::prepared_raw_segment binding) :
        _binding(std::move(binding)) {}
    /// Converts only emitted composite reads, retaining unused inputs as empty source unit.
    ///
    /// Complexity: O(arity + read payload size) time and canonical input space.
    template <std::size_t... I>
    auto bind(const std::tuple<const Args&...>& inputs, std::index_sequence<I...>) const {
        return std::tuple<detail::typed_input<Args>...>{
            detail::typed_input<Args>(std::get<I>(inputs), _binding._used[I])...};
    }
    /// Returns source-ordered views of used inputs and source-unit views of unused inputs.
    ///
    /// The inputs and binding must outlive the returned views.
    ///
    /// Complexity: O(arity) time and stack space.
    template <std::size_t... I>
    auto borrow(const std::tuple<detail::typed_input<Args>...>& inputs,
                std::index_sequence<I...>) const {
        return std::array<detail::borrowed_value, sizeof...(Args)>{
            (_binding._used[I] ? std::get<I>(inputs).borrow(_binding.argument_types()[I]) :
                                 detail::borrowed_value{_binding._unit, nullptr})...};
    }

public:
    prepared_segment(const prepared_segment&) = delete;
    prepared_segment& operator=(const prepared_segment&) = delete;
    /// Transfers typed binding ownership.
    prepared_segment(prepared_segment&&) noexcept = default;
    /// Transfers typed binding ownership.
    ///
    /// Complexity: O(old binding metadata and last-owned captures).
    prepared_segment& operator=(prepared_segment&&) noexcept = default;
    /// Borrows the frozen program's canonical frame reservation requirements.
    ///
    /// Composite conversions and arbitrary callback/nested costs are not inferred.
    ///
    /// \pre this handle has not been moved from.
    ///
    /// Example: `auto r = bound.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept { return _binding.requirements(); }
    /// Executes borrowed inputs and returns an independent concrete result.
    ///
    /// Owning vector/tuple payloads are disjoint; opaque pointer members retain their pointee
    /// aliases. Scalar calls allocate nothing on a warmed sufficiently reserved stack.
    ///
    /// \pre the handle is live, and inputs have no mutable alias accessed by callbacks.
    ///
    /// Exceptions propagate after suffix cleanup; inputs and caller prefix remain owned.
    /// Cleanup does not undo external callback effects.
    ///
    /// Complexity: O(arity + instructions + payload work) time and independent result space.
    ///
    /// Example: `bound.call(stack, 2.0);`.
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

// Adapter construction and checked binding follow the declaration's prepare() contract.
template <class R, class... Args>
prepared_segment<R, Args...> compiled_segment::prepare() const {
    static_assert(!std::is_void_v<R>, "use tuple<> for the source-unit result");
    const std::array<value_type, sizeof...(Args)> types{detail::concrete_adapter<Args>::type()...};
    return prepared_segment<R, Args...>(prepare_raw(types, detail::concrete_adapter<R>::type()));
}

/// @}
} // namespace cel
#endif
