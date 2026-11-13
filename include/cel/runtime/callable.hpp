// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CALLABLE_HPP
#define CEL_RUNTIME_CALLABLE_HPP
#include <cel/runtime/dyn_segment.hpp>

namespace cel {
/// \addtogroup runtime_callables
/// @{

namespace detail {
struct callable_state;
struct callable_access;
class callable_invocation;
} // namespace detail
/// A reference-semantic handle to frozen code and its complete declared signature.
///
/// Copies share code, explicit callback references, and the same-carrier entry guard.
/// There is no implicit lexical environment or shared invocation argument/result table.
/// Requires exclusive access to the carrier and any mutable callback state.
/// Distinct carriers can nest on the caller's stack. Callback references stay caller-owned.
/// Moves between distinct handles leave the source without code; assign a live handle before
/// using source's borrowed accessors. Borrowed signatures and requirements remain valid while
/// an unchanged live handle retains their code.
/// Last ownership retires runtime-managed executable graphs without allocation or recursive
/// descent, including callable literals and lazy bodies. Client destructors retain their costs.
///
/// Recursive entry into the same carrier throws std::logic_error.
///
/// Bind a declared parameter and call through a shared handle:
/// @code
/// cel::dyn_segment builder;
/// builder.push_argument(0, cel::native_type<double>());
/// const auto parameters = std::array{cel::native_type<double>()};
/// cel::callable fn(std::move(builder).finish(), parameters);
/// auto alias = fn;
/// cel::raw_stack stack;
/// const auto arguments = std::array{cel::value::from(2.0)};
/// auto result = alias.call(stack, arguments); // result.as<double>() == 2.0.
/// @endcode
class callable {
    detail::retirement_ptr<detail::callable_state> _state;
    friend struct detail::callable_access;
    friend class detail::callable_invocation;

public:
    /// Binds independent one-result code and every explicit source-order parameter.
    ///
    /// Retains code and copies parameter descriptors; the span need not outlive construction.
    /// Unused declarations remain parameters. Parameters admit bool/double and their composites;
    /// results admit every complete owned type, including client leaves and installed callables.
    ///
    /// \pre body has not been moved from.
    ///
    /// Throws std::invalid_argument for a dependent origin, undeclared or incompatible reads,
    /// or invalid parameter/result types.
    ///
    /// Complexity: O(parameter count + complete type-comparison work) time and parameter
    /// binding storage, plus comparison auxiliary space.
    ///
    /// Example: `callable fn(std::move(b).finish(), std::array{value_type::leaf<double>()});`.
    callable(compiled_segment body, std::span<const value_type> parameters);
    /// Shares the reference-semantic code handle, not an independent callback-state copy.
    ///
    /// Example: `auto alias = fn;`.
    callable(const callable&) noexcept = default;
    /// Transfers the code handle.
    ///
    /// Example: `auto moved = std::move(fn);`.
    callable(callable&&) noexcept = default;
    /// Shares the handle and releases the previous code on last ownership.
    ///
    /// Leaves the source unchanged; self-copy is unchanged.
    /// Complexity: O(last-owned captures and binding metadata).
    ///
    /// Example: `alias = fn;`.
    callable& operator=(const callable&) noexcept = default;
    /// Transfers the handle and releases previous last-owned code.
    ///
    /// Leaves a distinct source without code; self-move is unchanged.
    /// Complexity: O(last-owned captures and binding metadata).
    ///
    /// Example: `alias = std::move(fn);`.
    callable& operator=(callable&&) noexcept = default;
    /// Releases last-owned frozen code and signature metadata.
    ///
    /// Complexity: O(last-owned captures and binding metadata).
    ~callable();
    /// Borrows the full installed signature, including unused parameters.
    ///
    /// \pre this handle is live.
    ///
    /// Example: `auto arity = fn.type().parameters().size();`.
    const value_type& type() const noexcept;
    /// Borrows known body storage requirements; external callback costs are not inferred.
    ///
    /// \pre this handle is live.
    ///
    /// Example: `auto r = fn.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept;
    /// Executes declared positional inputs and returns one independent result owner.
    ///
    /// Validates arity and every complete parameter before body effects.
    /// Arguments are borrowed, not consumed. Returned aggregates own independent payloads;
    /// callable components share code, and opaque leaves do not acquire pointee ownership.
    /// Same-carrier exclusion covers validation, storage acquisition, body work, and result
    /// extraction.
    ///
    /// \pre callbacks do not mutate arguments or their owned payloads while this call borrows them.
    ///
    /// Throws std::invalid_argument for a signature mismatch; std::logic_error for a moved-from
    /// handle, same-carrier entry, or stack access during a canonical ownership phase.
    /// Exceptions propagate unchanged and retire only the execution suffix, preserving the caller
    /// prefix. Cleanup does not undo external callback effects.
    ///
    /// Complexity: O(parameter count + complete type-comparison work + executed instructions +
    /// payload and callback work) time. Allocates argument views and result outer storage,
    /// not per-scalar public owner boxes.
    ///
    /// Example: `auto result = fn.call(stack, std::array{value::from(2.0)});`.
    value call(raw_stack& stack, std::span<const value> arguments) const;
    /// Creates an owning value carrier sharing this explicitly reference-semantic code.
    ///
    /// Throws std::logic_error for a moved-from handle.
    ///
    /// Complexity: a constant number of allocations for constant-size carrier storage.
    ///
    /// Example: `auto carrier = fn.to_value();`.
    value to_value() const;
    /// Returns a shared handle to the installed callable stored in source.
    ///
    /// The returned handle shares the value's reference-semantic code and entry guard.
    /// Leaves source unchanged and retains code independently of source's lifetime.
    /// Performs no payload clone or recursive signature comparison.
    ///
    /// Throws std::invalid_argument if source is not a callable with runtime-installed ownership.
    ///
    /// Example: `auto fn = callable::from_value(factory.call(stack, {}));`.
    static callable from_value(const value& source);
};

/// @}
} // namespace cel
#endif
