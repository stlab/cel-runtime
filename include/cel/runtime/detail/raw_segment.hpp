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
/// Owns a move-only stored operation sequence and stable, correctly constructed captures.
/// Execution is const; construction and movement require exclusive access.
/// Trusted-program precondition: every instruction finds its complete ordered signature
/// above the frame checkpoint, including matching canonical ownership operations.
/// Descriptors/captures are retained; referenced external state and argument spans are borrowed.
/// Sharing code does not make externally mutable callbacks thread-safe.
///
/// Store a constant and a unary operation, then execute them in one scope:
/// @code
/// cel::raw_segment program;
/// program.push_op0<double>([](cel::frame&) { return 2.0; });
/// program.push_op1<double, double>([](cel::frame&, double n) { return n * 3.0; });
/// cel::raw_stack stack;
/// cel::frame scope(stack);
/// program.execute(scope);
/// auto result = scope.pop<double>(); // 6.0; the scope's suffix is empty.
/// @endcode
class raw_segment {
    detail::retirement_ptr<detail::instruction_storage> _storage;
    friend struct detail::segment_access;

    /// Allocates stable, aligned capture storage using a standard arena.
    void* allocate_capture(std::size_t size, std::size_t alignment);
    /// Publishes one constructed capture and its function-pointer dispatch/destructor.
    /// Failure leaves ownership with the caller; complexity is amortized O(1).
    void append(const void* capture,
                void (*execute)(const void*, frame&),
                void (*destroy)(const void*) noexcept,
                void (*retire)(const void*, detail::retirement_queue&) noexcept = nullptr);

    /// Owns typed argument/result metadata and a real C++ capture object.
    template <class F, class R, class... Args>
    struct operation {
        std::array<value_type, sizeof...(Args)> inputs; ///< Retains complete operand types.
        std::array<value_type, std::is_void_v<R> ? 0 : 1>
            outputs; ///< Retains complete result type.
        F callback;  ///< Owns the callback object; never byte-relocated.
        /// Prepares zero or one complete result descriptors during construction.
        static auto result_types() {
            if constexpr (std::is_void_v<R>)
                return std::array<value_type, 0>{};
            else
                return std::array{detail::concrete_adapter<R>::type()};
        }
        /// Constructs signature metadata and an owned callback from its source.
        template <class G>
        requires std::constructible_from<F, G&&> explicit operation(G&& input) :
            inputs{detail::concrete_adapter<Args>::type()...}, outputs(result_types()),
            callback(std::forward<G>(input)) {}
        /// Reads arguments in source order before retiring canonical operands.
        /// Complexity: O(operand payload size) time and concrete argument space.
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
        /// Dispatches one stored operation through a typed function-pointer thunk.
        /// Complexity: O(operand/result payload work).
        static void execute(const void* capture, frame& scope) {
            static_cast<const operation*>(capture)->invoke(scope,
                                                           std::index_sequence_for<Args...>{});
        }
        /// Destroys the real typed capture object without relocating it.
        static void destroy(const void* capture) noexcept {
            std::destroy_at(static_cast<const operation*>(capture));
        }
    };

public:
    /// Creates an empty no-op sequence.
    raw_segment() noexcept;
    /// Destroys every capture exactly once and releases its arena.
    /// Complexity: O(instructions + capture destruction work).
    ~raw_segment();
    /// Disallows copied mutable construction ownership.
    raw_segment(const raw_segment&) = delete;
    /// Disallows copied assignment.
    raw_segment& operator=(const raw_segment&) = delete;
    /// Transfers the stable arena and leaves other an empty reusable sequence.
    raw_segment(raw_segment&& other) noexcept;
    /// Releases old captures and transfers the arena; self-move is unchanged.
    /// Complexity: O(old instructions + old capture destruction work).
    raw_segment& operator=(raw_segment&& other) noexcept;
    /// Executes in order with nonallocating runtime dispatch for reserved scalar programs.
    /// Client callback work and owning composite results have their own costs.
    /// Precondition: the trusted program's complete signatures match the live suffix.
    /// An instruction exception retires this frame's whole suffix and preserves its prefix.
    /// Callback exceptions propagate unchanged; cleanup does not undo external callback effects.
    /// Throws logic_error before execution if a nested frame or canonical phase is active.
    /// Rejected entry leaves the suffix unchanged.
    /// Complexity: O(instructions + operand/result payload work).
    /// Example: `program.execute(scope);`.
    // [knowledge] Retain release-build depth/layout checks even for trusted instruction signatures.
    void execute(frame& scope) const;
    /// Appends an arbitrary-arity typed operation with source-order operands.
    /// The stored callback is invoked as const decay_t<F>&(frame&, Args...) and initializes R,
    /// or R is void.
    /// void consumes operands and produces no operand; tuple<> produces source unit.
    /// Capture ownership is independent of the input object after successful construction.
    /// Lvalue callbacks are copied; rvalue callbacks are moved into owned decayed storage.
    /// References or pointers held by a capture keep their original ownership and lifetime.
    /// The callback receives independently materialized operands after their stack owners retire;
    /// it may use the active frame or a nested frame. Its exception propagates from execute.
    /// Captures must have nonthrowing destructors; operands/results use value's concrete adapters.
    /// Prepares scalar descriptors during construction, not execution.
    /// Complexity: O(arity + capture construction) time and stored space.
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
    /// Complexity: O(capture construction) time and space.
    /// Example: `p.push_op0<bool>([](frame&) { return true; });`.
    template <class R, class F>
    void push_op0(F&& callback) {
        push_op<R>(std::forward<F>(callback));
    }
    /// Appends a unary operation under push_op's capture and invocation contract.
    /// Complexity: O(capture construction) time and space.
    /// Example: `p.push_op1<bool, bool>([](frame&, bool b) { return !b; });`.
    template <class R, class A, class F>
    void push_op1(F&& callback) {
        push_op<R, A>(std::forward<F>(callback));
    }
    /// Appends a binary operation, with the older operand first.
    /// Follows push_op's capture, invocation, and exception contract.
    /// Complexity: O(capture construction) time and space.
    /// Example: `p.push_op2<double, double, double>(
    /// [](frame&, double a, double b) { return a / b; });`.
    template <class R, class A, class B, class F>
    void push_op2(F&& callback) {
        push_op<R, A, B>(std::forward<F>(callback));
    }
    /// Appends a ternary operation, with operands in source order.
    /// Follows push_op's capture, invocation, and exception contract.
    /// Complexity: O(capture construction) time and space.
    /// Example: `p.push_op3<double, double, double, double>(
    /// [](frame&, double a, double b, double c) { return a - b - c; });`.
    template <class R, class A, class B, class C, class F>
    void push_op3(F&& callback) {
        push_op<R, A, B, C>(std::forward<F>(callback));
    }
};
} // namespace cel::detail
#endif
