// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CALLABLE_HPP
#define CEL_RUNTIME_CALLABLE_HPP
#include <cel/runtime/dyn_segment.hpp>

namespace cel {
namespace detail {
struct callable_state;
struct callable_access;
class callable_invocation;
} // namespace detail
/// Owns a reference-semantic handle to frozen code and its complete declared signature.
/// Copies share code, explicit callback references, and the same-carrier entry guard.
/// There is no implicit lexical environment or shared invocation argument/result table.
/// Exclusive access is required; recursive entry into the same carrier throws logic_error.
/// Distinct carriers can nest on the caller's stack. Callback references stay caller-owned.
/// Last ownership retires runtime-managed executable graphs without allocation or recursive
/// descent, including callable literals and lazy bodies. Client destructors retain their costs.
/// Example: `callable fn(std::move(builder).finish(), parameters); auto v = fn.call(stack, args);`.
class callable {
    detail::retirement_ptr<detail::callable_state> _state;
    friend struct detail::callable_access;
    friend class detail::callable_invocation;

public:
    /// Binds independent one-result code and every explicit source-order parameter.
    /// Unused declarations remain parameters. Parameters admit bool/f64 and their composites;
    /// results admit every complete owned type, including client leaves and installed callables.
    /// Throws invalid_argument for dependent origin, undeclared/wrong reads, or invalid types.
    /// Complexity: O(parameters + compared metadata) time and frozen binding space.
    /// Example: `callable fn(std::move(b).finish(), std::array{value_type::leaf<double>()});`.
    callable(compiled_segment body, std::span<const value_type> parameters);
    /// Shares the reference-semantic code handle, not an independent callback-state copy.
    /// Example: `auto alias = fn;`.
    callable(const callable&) noexcept = default;
    /// Transfers the code handle.
    /// Example: `auto moved = std::move(fn);`.
    callable(callable&&) noexcept = default;
    /// Shares the handle and releases the previous code on last ownership.
    /// Complexity: O(last-owned captures and binding metadata).
    /// Example: `alias = fn;`.
    callable& operator=(const callable&) noexcept = default;
    /// Transfers the handle and releases previous last-owned code.
    /// Complexity: O(last-owned captures and binding metadata).
    /// Example: `alias = std::move(fn);`.
    callable& operator=(callable&&) noexcept = default;
    /// Releases last-owned frozen code and signature metadata.
    /// Complexity: O(last-owned captures and binding metadata).
    ~callable();
    /// Borrows the full installed signature, including unused parameters.
    /// Precondition: this handle is live.
    /// Example: `auto arity = fn.type().parameters().size();`.
    const value_type& type() const noexcept;
    /// Borrows known body storage requirements; external callback costs are not inferred.
    /// Precondition: this handle is live.
    /// Example: `auto r = fn.requirements().for_prefix(stack.cursor(), stack.depth());`.
    const stack_requirements& requirements() const noexcept;
    /// Validates arity and every declared complete parameter before any body effect.
    /// Returns one independent result owner; arguments are borrowed and not consumed.
    /// Throws invalid_argument on any signature mismatch and logic_error on same-carrier entry.
    /// Same-carrier exclusion covers validation, storage acquisition, body work, and result
    /// extraction. Exceptions preserve their context and retire only the execution suffix.
    /// Callbacks must not mutate aliases of arguments while this call borrows them.
    /// Complexity: O(parameters + compared metadata + executed payload work) time;
    /// allocates an invocation table and result outer storage, not per-scalar argument boxes.
    /// Example: `auto result = fn.call(stack, std::array{value::from(2.0)});`.
    value call(raw_stack& stack, std::span<const value> arguments) const;
    /// Creates an owning value carrier sharing this explicitly reference-semantic code.
    /// Throws logic_error for a moved-from handle.
    /// Complexity: O(1) allocations for carrier and canonical outer storage.
    /// Example: `auto carrier = fn.to_value();`.
    value to_value() const;
    /// Receives an installed owning callable value through public headers.
    /// Throws invalid_argument for a different complete shape or ownership representation.
    /// The returned handle shares the value's reference-semantic code and entry guard.
    /// Complexity: O(compared signature metadata); no payload clone.
    /// Example: `auto fn = callable::from_value(factory.call(stack, {}));`.
    static callable from_value(const value& source);
};
} // namespace cel
#endif
