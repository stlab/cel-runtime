// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_RAW_SEGMENT_HPP
#define CEL_RUNTIME_RAW_SEGMENT_HPP
#include <array>
#include <cel/runtime/detail/frame.hpp>
#include <cel/runtime/detail/retirement.hpp>
#include <concepts>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>

namespace cel::detail {
struct instruction_storage;
struct segment_access;
/// A move-only operation sequence with stable, owned captures.
///
/// Execution is const; construction and movement require exclusive access.
/// Descriptors/captures are retained; referenced external state and argument spans are borrowed.
/// Sharing code does not make externally mutable callbacks thread-safe.
///
/// \pre every executed instruction finds its complete ordered signature above the frame
/// checkpoint, including matching canonical ownership operations.
///
/// Store a constant and a unary operation, then execute them in one scope:
/// @code
/// cel::detail::raw_segment program;
/// program.push_op0<double>([](cel::detail::frame&) { return 2.0; });
/// program.push_op1<double, double>([](cel::detail::frame&, double n) { return n * 3.0; });
/// cel::raw_stack stack;
/// cel::detail::frame scope(stack);
/// program.execute(scope);
/// auto result = scope.pop<double>(); // 6.0; the scope's suffix is empty.
/// @endcode
class raw_segment {
    detail::retirement_ptr<detail::instruction_storage> _storage;
    friend struct detail::segment_access;

    /// Returns stable, aligned, uninitialized capture storage owned by this sequence.
    ///
    /// \pre alignment is a nonzero power of two.
    ///
    /// The storage remains valid until the sequence's capture storage is retired.
    /// Complexity: size bytes of result storage, plus allocator work.
    void* allocate_capture(std::size_t size, std::size_t alignment);
    /// Appends dispatch and retirement for one constructed capture, taking its ownership.
    ///
    /// \pre capture is a live object in this sequence's capture storage; the supplied operations
    /// accept that object, and destruction and retirement do not throw.
    ///
    /// Failure leaves capture ownership with the caller.
    ///
    /// Complexity: amortized constant time and record storage; worst-case time linear in the
    /// current instruction count on capacity growth.
    void append(const void* capture,
                void (*execute)(const void*, frame&),
                void (*destroy)(const void*) noexcept,
                void (*retire)(const void*, detail::retirement_queue&) noexcept = nullptr);

    /// An owned callback with complete operand and result descriptors.
    template <class F, class R, class... Args>
    struct operation {
        std::array<value_type, sizeof...(Args)> inputs; ///< Retains complete operand types.
        std::array<value_type, std::is_void_v<R> ? 0 : 1>
            outputs; ///< Retains complete result type.
        F callback;  ///< Owns the callback object; never byte-relocated.
        /// Returns no result descriptor for void, or the complete concrete R descriptor.
        ///
        /// Complexity: R's concrete descriptor construction costs.
        static auto result_types() {
            if constexpr (std::is_void_v<R>)
                return std::array<value_type, 0>{};
            else
                return std::array{detail::concrete_adapter<R>::type()};
        }
        /// Constructs signature metadata and an owned callback from its source.
        ///
        /// Complexity: operand/result descriptor construction and callback construction costs.
        template <class G>
        requires std::constructible_from<F, G&&> explicit operation(G&& input) :
            inputs{detail::concrete_adapter<Args>::type()...}, outputs(result_types()),
            callback(std::forward<G>(input)) {}
        /// Consumes source-ordered operands, invokes the callback, and publishes its result.
        ///
        /// Materializes independent concrete operands before retiring their canonical owners.
        /// Callback exceptions propagate without a published result.
        ///
        /// Complexity: operand/result adaptation, operand retirement, and callback work;
        /// independent concrete argument/result storage.
        template <std::size_t... I>
        void invoke(frame& scope, std::index_sequence<I...>) const {
            auto arguments =
                scope.template acquire_operands<Args...>(inputs, std::index_sequence<I...>{});
            scope.discard(sizeof...(Args));
            if constexpr (std::is_void_v<R>) {
                std::apply([&](auto&... args) { std::invoke(callback, scope, std::move(args)...); },
                           arguments);
            } else {
                R result = std::apply(
                    [&](auto&... args) -> R {
                        return std::invoke(callback, scope, std::move(args)...);
                    },
                    arguments);
                scope.push_result(outputs[0], result);
            }
        }
        /// Executes the captured operation under invoke's ownership and failure contract.
        ///
        /// \pre capture points to a live operation of this specialization.
        ///
        /// Complexity: invoke's adaptation, retirement, and callback costs.
        static void execute(const void* capture, frame& scope) {
            static_cast<const operation*>(capture)->invoke(scope,
                                                           std::index_sequence_for<Args...>{});
        }
        /// Destroys the captured operation.
        ///
        /// \pre capture points to an exclusively owned live operation of this specialization.
        ///
        /// Complexity: owned descriptor and callback destruction costs.
        static void destroy(const void* capture) noexcept {
            std::destroy_at(static_cast<const operation*>(capture));
        }
    };

public:
    /// Creates an empty no-op sequence.
    raw_segment() noexcept;
    /// Destroys every capture exactly once and releases its arena.
    ///
    /// Retires runtime-managed descendants without allocation or nesting-dependent stack.
    ///
    /// Complexity: instruction traversal and last-owned capture/code/metadata destruction costs.
    ~raw_segment();
    raw_segment(const raw_segment&) = delete;
    raw_segment& operator=(const raw_segment&) = delete;
    /// Transfers the sequence and leaves other empty and reusable.
    raw_segment(raw_segment&& other) noexcept;
    /// Releases prior captures and transfers the source sequence.
    ///
    /// Self-move is unchanged; a distinct source becomes empty and reusable.
    ///
    /// Complexity: prior instruction traversal and last-owned capture/code/metadata destruction.
    raw_segment& operator=(raw_segment&& other) noexcept;
    /// Executes the sequence in order within the supplied frame.
    ///
    /// Reserved scalar dispatch does not allocate.
    /// Client callback work and owning composite results have their own costs.
    ///
    /// \pre the trusted program's complete signatures match the live suffix.
    ///
    /// An instruction exception retires this frame's whole suffix and preserves its prefix.
    /// Callback exceptions propagate unchanged; cleanup does not undo external callback effects.
    /// Throws std::logic_error before execution if a nested frame or canonical phase is active.
    /// Rejected entry leaves the suffix unchanged.
    ///
    /// Complexity: instruction dispatch plus operand/result adaptation, retirement, and callback
    /// work. Exception cleanup includes the retired suffix's ownership costs.
    ///
    /// Example: `program.execute(scope);`.
    // [knowledge] Retain release-build depth/layout checks even for trusted instruction signatures.
    void execute(frame& scope) const;
    /// Appends an arbitrary-arity typed operation with source-order operands.
    ///
    /// Invokes the stored callback as const decay_t<F>&(frame&, Args...).
    /// void consumes operands and produces no operand; tuple<> produces source unit.
    /// Capture ownership is independent of the input object after successful construction.
    /// Lvalue callbacks are copied; rvalue callbacks are moved into owned decayed storage.
    /// References or pointers held by a capture keep their original ownership and lifetime.
    /// The callback receives independently materialized operands after their stack owners retire;
    /// it may use the active frame or a nested frame. Its exception propagates from execute.
    /// Captures require nonthrowing destruction; operands/results use value's concrete adapters.
    /// Scalar descriptors are prepared during construction, not execution.
    ///
    /// Failure does not publish the operation or leave a live unowned capture.
    ///
    /// Complexity: operand/result descriptor and callback construction costs, plus append's
    /// amortized bookkeeping costs; signature and capture storage.
    ///
    /// Example: `p.push_op<double, double>([](frame&, double n) { return n * 2.0; });`.
    template <class R, class... Args, class F>
    void push_op(F&& callback) {
        using capture_type = std::decay_t<F>;
        static_assert(std::is_nothrow_destructible_v<capture_type>);
        static_assert(std::is_invocable_r_v<R, const capture_type&, frame&, Args...>);
        using stored = operation<capture_type, R, Args...>;
        auto* location = static_cast<stored*>(allocate_capture(sizeof(stored), alignof(stored)));
        auto* capture = std::construct_at(location, std::forward<F>(callback));
        try {
            append(capture, stored::execute, stored::destroy);
        } catch (...) {
            std::destroy_at(capture);
            throw;
        }
    }
    /// Appends a zero-operand operation under push_op's capture and invocation contract.
    ///
    /// Follows push_op's failure and complexity contracts.
    ///
    /// Example: `p.push_op0<bool>([](frame&) { return true; });`.
    template <class R, class F>
    void push_op0(F&& callback) {
        push_op<R>(std::forward<F>(callback));
    }
    /// Appends a unary operation under push_op's capture and invocation contract.
    ///
    /// Follows push_op's failure and complexity contracts.
    ///
    /// Example: `p.push_op1<bool, bool>([](frame&, bool b) { return !b; });`.
    template <class R, class A, class F>
    void push_op1(F&& callback) {
        push_op<R, A>(std::forward<F>(callback));
    }
    /// Appends a binary operation, with the older operand first.
    ///
    /// Follows push_op's capture, invocation, failure, and complexity contracts.
    ///
    /// Example: `p.push_op2<double, double, double>(
    /// [](frame&, double a, double b) { return a / b; });`.
    template <class R, class A, class B, class F>
    void push_op2(F&& callback) {
        push_op<R, A, B>(std::forward<F>(callback));
    }
    /// Appends a ternary operation, with operands in source order.
    ///
    /// Follows push_op's capture, invocation, failure, and complexity contracts.
    ///
    /// Example: `p.push_op3<double, double, double, double>(
    /// [](frame&, double a, double b, double c) { return a - b - c; });`.
    template <class R, class A, class B, class C, class F>
    void push_op3(F&& callback) {
        push_op<R, A, B, C>(std::forward<F>(callback));
    }
};
} // namespace cel::detail
#endif
