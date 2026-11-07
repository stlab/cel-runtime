// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CALLABLE_ACCESS_HPP
#define CEL_RUNTIME_CALLABLE_ACCESS_HPP
#include <cel/runtime/callable.hpp>
namespace cel::detail {
/// Owns a stable code handle and excludes same-carrier entry for the entire invocation.
/// Declares before staging/result guards so their cleanup precedes entry retirement.
class callable_invocation {
    callable _code; ///< Retains the guarded immutable code independently of stack carriers.

public:
    /// Retains reference-semantic code and enters its guard without allocating on success.
    /// Throws logic_error for a moved-from handle or already-active carrier.
    explicit callable_invocation(callable code);
    /// Restores idle state after invocation-owned staging/results retire, then releases code.
    /// Gate restoration never allocates or throws.
    /// Complexity: O(last-owned captures and binding metadata).
    ~callable_invocation() noexcept;
    /// Disallows duplicated entry ownership.
    callable_invocation(const callable_invocation&) = delete;
    /// Disallows duplicated entry assignment.
    callable_invocation& operator=(const callable_invocation&) = delete;
    /// Dispatches trusted stable canonical arguments through an already-entered body.
    /// Precondition: complete arity/ownership were bound; inputs/output are disjoint from
    /// mutable stack bytes and remain live/aligned through the call.
    /// Failure leaves output uninitialized; original body exceptions propagate.
    /// Complexity: O(arguments + executed instructions and payload work).
    void call_into(raw_stack& stack,
                   std::span<const borrowed_value> arguments,
                   std::byte* destination) const;
};
/// Dispatches installed carriers with construction-bound complete ownership witnesses.
struct callable_access {
    /// Borrows an installed carrier handle from initialized canonical bytes.
    /// Precondition: bytes contain this implementation's live complete carrier.
    static const callable& read(const std::byte* bytes) noexcept;
    /// Reports whether the descriptor has this implementation's complete ownership pair.
    static bool installed(const value_type& type) noexcept;
};
} // namespace cel::detail
#endif
