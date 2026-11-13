// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CALLABLE_ACCESS_HPP
#define CEL_RUNTIME_CALLABLE_ACCESS_HPP
#include <cel/runtime/callable.hpp>
namespace cel::detail {
/// A retained code handle that excludes same-carrier entry for its lifetime.
///
/// Must outlive invocation-owned argument staging and result publication/cleanup.
class callable_invocation {
    callable _code; // Retains guarded code independently of stack carriers.

public:
    /// Retains reference-semantic code and enters its guard without allocating on success.
    ///
    /// Throws std::logic_error for a moved-from handle or already-active carrier.
    explicit callable_invocation(callable code);
    /// Releases the entry guard and retained code.
    ///
    /// Guard release does not allocate.
    ///
    /// Complexity: O(last-owned captures and binding metadata).
    ~callable_invocation() noexcept;
    callable_invocation(const callable_invocation&) = delete;
    callable_invocation& operator=(const callable_invocation&) = delete;
    /// Executes trusted canonical arguments through the guarded body.
    ///
    /// \pre emitted reads match their bound complete types and ownership operations.
    /// Their initialized bytes remain live, aligned, and disjoint from mutable stack/output
    /// storage; no callback accesses mutable aliases of the inputs.
    /// \pre destination is uninitialized, aligned, disjoint storage for the complete result;
    /// null is permitted for a zero-byte result.
    ///
    /// Throws std::invalid_argument for wrong arity or std::logic_error for canonical-phase
    /// stack access. Failure leaves output uninitialized and preserves the caller prefix.
    /// Body exceptions propagate unchanged; external callback effects are not rolled back.
    ///
    /// Complexity: argument views, executed instructions, payload/callback work, and suffix
    /// cleanup.
    void call_into(raw_stack& stack,
                   std::span<const borrowed_value> arguments,
                   std::byte* destination) const;
};
/// Access to the runtime's installed callable carriers.
struct callable_access {
    /// Borrows an installed carrier handle from initialized canonical bytes.
    ///
    /// \pre bytes contain this implementation's live complete carrier, whose owner remains
    /// unchanged and alive while the returned handle is borrowed.
    static const callable& read(const std::byte* bytes) noexcept;
    /// Returns whether type has the runtime's installed callable carrier operations.
    ///
    /// Does not establish the presence of initialized carrier bytes.
    static bool installed(const value_type& type) noexcept;
    /// Retires the installed carrier's owned handle without freeing its outer bytes.
    ///
    /// Leaves the carrier bytes uninitialized.
    ///
    /// \pre bytes contain this implementation's exclusively owned live complete carrier.
    static void retire(std::byte* bytes, retirement_queue& queue) noexcept;
};
} // namespace cel::detail
#endif
