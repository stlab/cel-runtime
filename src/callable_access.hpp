// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CALLABLE_ACCESS_HPP
#define CEL_RUNTIME_CALLABLE_ACCESS_HPP
#include <cel/runtime/callable.hpp>
namespace cel::detail {
/// Dispatches installed carriers with construction-bound complete ownership witnesses.
struct callable_access {
    /// Calls trusted stable canonical arguments into aligned uninitialized result bytes.
    /// Precondition: signature/ownership and complete arity were bound by the emitter;
    /// source bytes are stable, disjoint from stack/output, and live through dispatch.
    /// Throws logic_error on same-carrier entry; failure leaves output uninitialized.
    /// Complexity: O(arguments + executed instructions and payload work).
    static void call_into(const callable& fn,
                          raw_stack& stack,
                          std::span<const borrowed_value> arguments,
                          std::byte* destination);
    /// Borrows an installed carrier handle from initialized canonical bytes.
    /// Precondition: bytes contain this implementation's live complete carrier.
    static const callable& read(const std::byte* bytes) noexcept;
    /// Reports whether the descriptor has this implementation's complete ownership pair.
    static bool installed(const value_type& type) noexcept;
};
} // namespace cel::detail
#endif
