// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_TYPE_OPS_HPP
#define CEL_RUNTIME_TYPE_OPS_HPP

#include <cel/runtime/value_type.hpp>

namespace cel::detail {

/// Supplies a complete ownership pair for an opaque byte-transferable callable carrier.
/// clone creates independent mutable ownership; destroy never allocates or throws.
/// A throwing clone must release its partial construction and leave destination uninitialized.
/// Both functions operate on the signature's canonical extent; no operand move callback exists.
struct opaque_ownership {
    void (*clone)(const std::byte*, std::byte*) = nullptr; ///< Clones one carrier.
    void (*destroy)(std::byte*) noexcept = nullptr;        ///< Destroys one carrier.
};
static_assert(std::is_trivially_copyable_v<opaque_ownership>);

/// Installs immutable callable ownership metadata without changing semantic signature identity.
/// These operations do not implement callable execution or concrete closure bodies.
struct type_access {
    /// Copies one metadata plan and installs a complete ownership pair.
    /// Throws std::invalid_argument for non-callables or missing operations.
    /// Caller supplies trusted operations satisfying opaque_ownership's contract.
    /// Complexity: O(parameter count) time and metadata space; nested descriptors stay shared.
    static value_type install_callable_ownership(const value_type& signature,
                                                 opaque_ownership operations);
    /// Returns the installed pair, or an empty pair for a deferred callable.
    /// Precondition: type is callable.
    static opaque_ownership operations(const value_type& type) noexcept;
    /// Compares installed carrier operations, short-circuiting shared immutable plans.
    /// Precondition: complete semantic shapes already match.
    /// Complexity: O(unshared payload shape size) time and O(shape depth) call space.
    static bool compatible_operations(const value_type& expected, const value_type& actual);
};

/// Validates complete type-level cloning and destruction capabilities.
/// Throws std::invalid_argument when any required opaque ownership operation is missing.
/// Empty arrays can own values even when their descriptor fails this conservative check.
void require_ownership(const value_type& type);

/// Validates complete shape and compatible opaque carrier ownership operations.
/// Semantic identity alone never authorizes using one opaque representation's operations
/// on another. Throws std::invalid_argument for shape or installed-operation mismatch.
/// Complexity: O(combined shape size) time and O(shape depth) call space.
void require_compatible(const value_type& expected, const value_type& actual);

} // namespace cel::detail
#endif
