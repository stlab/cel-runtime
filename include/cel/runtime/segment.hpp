// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_SEGMENT_HPP
#define CEL_RUNTIME_SEGMENT_HPP
#include <cel/runtime/dyn_segment.hpp>

namespace cel {
/// Describes source-order types without assuming any physical tuple layout.
/// The first type is the oldest operand; an empty list contains no operands.
/// Example: `type_list<double, bool>` describes two ordered operands.
template <class... T>
struct type_list {};
namespace detail {
/// Maps a standard compile-time tuple to a stack type list.
template <class T>
struct tuple_list;
/// Extracts source-order tuple types.
template <class... T>
struct tuple_list<std::tuple<T...>> {
    using type = type_list<T...>; ///< Describes the same source-order stack.
};
/// Computes available suffixes and resulting static stack effects.
template <class T>
struct static_stack;
/// Uses standard tuple/index-sequence facilities rather than runtime native tuple layouts.
template <class... T>
struct static_stack<type_list<T...>> {
    using tuple = std::tuple<T...>; ///< Supplies compile-time indexed type access.
    /// Selects indexed types without constructing any tuple objects.
    template <class I>
    struct prefix_tuple;
    /// Maps a source-order prefix to a standard compile-time tuple.
    template <std::size_t... I>
    struct prefix_tuple<std::index_sequence<I...>> {
        using type = std::tuple<std::tuple_element_t<I, tuple>...>; ///< Retains prefix types.
    };
    /// Tests exact source-order operand availability.
    template <class... A, std::size_t... I>
    static consteval bool matches_at(std::index_sequence<I...>) {
        return (std::same_as<A, std::tuple_element_t<sizeof...(T) - sizeof...(A) + I, tuple>> &&
                ...);
    }
    /// Rejects unavailable operands before instantiating indexed tuple access.
    template <class... A>
    static consteval bool matches() {
        if constexpr (sizeof...(A) > sizeof...(T))
            return false;
        else
            return matches_at<A...>(std::index_sequence_for<A...>{});
    }
    /// Computes consumed-prefix plus zero or one output type.
    template <class R, std::size_t N>
    using effect = typename tuple_list<decltype(std::tuple_cat(
        std::declval<typename prefix_tuple<std::make_index_sequence<sizeof...(T) - N>>::type>(),
        std::declval<std::conditional_t<std::is_void_v<R>, std::tuple<>, std::tuple<R>>>()))>::type;
};
} // namespace detail

/// Owns a checked builder whose template stack matches its physical logical operands.
/// Initial arguments are seeded as owned emitted reads of borrowed inputs, in source order.
/// Only matching initial Args/Stack instances can be constructed directly; other stack
/// types result from checked operations, not template ascription of unrelated origins.
/// Finalization requires one operand or source unit, never silent leftover discard.
/// Moving transfers construction ownership; use the source only for destruction or assignment.
/// Destruction releases unfinished captures and metadata.
/// Complexity: destruction takes O(owned captures, metadata, and client destructor work).
/// Example: `segment<type_list<double>> b; auto p = std::move(b).finish();`.
///
/// Replace the seeded input and prepare a typed call:
/// @code
/// cel::segment<cel::type_list<double>> builder;
/// auto next = std::move(builder).op1<double>([](double n) { return n * 2.0; });
/// auto program = std::move(next).finish();
/// auto bound = program.prepare<double, double>();
/// cel::raw_stack stack;
/// auto result = bound.call(stack, 3.0); // 6.0; the call borrows its input.
/// @endcode
template <class Args, class Stack = Args>
class segment;
/// Implements typed source-order stack flow using ordinary checked construction.
/// Follows segment's ownership and example; Stack lists current operands, oldest first.
template <class... Args, class... Stack>
class segment<type_list<Args...>, type_list<Stack...>> {
    dyn_segment _builder;
    template <class, class>
    friend class segment;
    /// Transfers a checked body after its static stack effect.
    explicit segment(dyn_segment builder) noexcept : _builder(std::move(builder)) {}
    /// Seeds physical initial operands as emitted owned argument reads.
    /// Complexity: O(argument count + metadata preparation).
    template <std::size_t... I>
    void seed(std::index_sequence<I...>) {
        (_builder.push_argument(I, detail::concrete_adapter<Args>::type()), ...);
    }
    /// Infers exact current suffix types for arity convenience operations.
    /// Precondition: N operands exist, established by public constraints.
    /// Complexity: O(arity + compared metadata + capture construction).
    template <class R, std::size_t N, class F, std::size_t... I>
    auto top(F&& callback, std::index_sequence<I...>) && {
        return std::move(*this)
            .template op<R,
                         std::tuple_element_t<sizeof...(Stack) - N + I, std::tuple<Stack...>>...>(
                std::forward<F>(callback));
    }

public:
    /// Constructs the declared initial stack from borrowed arguments at execution time.
    /// Complexity: O(argument count + metadata preparation).
    /// Example: `segment<type_list<double, bool>> builder;`.
    segment() requires std::same_as<type_list<Args...>, type_list<Stack...>> {
        seed(std::index_sequence_for<Args...>{});
    }
    /// Disallows copying mutable construction ownership.
    segment(const segment&) = delete;
    /// Disallows copied assignment.
    segment& operator=(const segment&) = delete;
    /// Transfers construction ownership and its static stack witness.
    segment(segment&&) noexcept = default;
    /// Transfers ownership and releases prior captures.
    /// Complexity: O(old captures and metadata).
    segment& operator=(segment&&) noexcept = default;
    /// Consumes an exact declared suffix and produces R, or no operand for void.
    /// Returns a builder with the consumed suffix replaced by R, omitting R when it is void.
    /// Follows dyn_segment::op's exact callback, capture lifetime, and construction error contract.
    /// Complexity: O(arity + compared metadata + capture construction), plus O(current stack depth)
    /// metadata transfer on growth.
    /// Example: `auto next = std::move(b).op<double, double>([](double n) { return n; });`.
    template <class R, class... A, class F>
    requires(detail::static_stack<type_list<Stack...>>::template matches<A...>() &&
             detail::exact_callback<F, R, A...>()) auto op(F&& callback) && {
        _builder.template op<R, A...>(std::forward<F>(callback));
        using output =
            typename detail::static_stack<type_list<Stack...>>::template effect<R, sizeof...(A)>;
        return segment<type_list<Args...>, output>(std::move(_builder));
    }
    /// Emits a callback without consuming operands and pushes R unless R is void.
    /// Follows op's callback, ownership, and exception contract.
    /// Complexity: O(capture construction), plus O(current stack depth) on metadata growth.
    /// Example: `auto next = std::move(b).op0<bool>([] { return true; });`.
    template <class R, class F>
    auto op0(F&& callback) && {
        return std::move(*this).template op<R>(std::forward<F>(callback));
    }
    /// Consumes the top operand using its statically known type and pushes R unless R is void.
    /// Follows op's callback, ownership, and exception contract.
    /// Complexity: O(compared metadata + capture construction).
    /// Example: `auto next = std::move(b).op1<bool>([](double n) { return n > 0.0; });`.
    template <class R, class F>
    requires(sizeof...(Stack) >= 1) auto op1(F&& callback) && {
        return std::move(*this).template top<R, 1>(std::forward<F>(callback),
                                                   std::make_index_sequence<1>{});
    }
    /// Consumes two older-first operands using their statically known types.
    /// Pushes R unless R is void; follows op's callback, ownership, and exception contract.
    /// Complexity: O(compared metadata + capture construction).
    /// Example: `auto next = std::move(b).op2<double>([](double a, double b) { return a - b; });`.
    template <class R, class F>
    requires(sizeof...(Stack) >= 2) auto op2(F&& callback) && {
        return std::move(*this).template top<R, 2>(std::forward<F>(callback),
                                                   std::make_index_sequence<2>{});
    }
    /// Consumes three source-order operands using their statically known types.
    /// Pushes R unless R is void; follows op's callback, ownership, and exception contract.
    /// Complexity: O(compared metadata + capture construction).
    /// Example: `auto next = std::move(b).op3<double>(
    /// [](double a, double b, double c) { return a - b - c; });`.
    template <class R, class F>
    requires(sizeof...(Stack) >= 3) auto op3(F&& callback) && {
        return std::move(*this).template top<R, 3>(std::forward<F>(callback),
                                                   std::make_index_sequence<3>{});
    }
    /// Freezes exactly one static result, or source unit for an empty stack.
    /// This overload is unavailable for leftover operands.
    /// Complexity: O(1), except frozen-engine allocation and source-unit emission.
    /// Example: `auto p = std::move(builder).finish();`.
    compiled_segment finish() && requires(sizeof...(Stack) <= 1) {
        return std::move(_builder).finish();
    }
};
} // namespace cel
#endif
