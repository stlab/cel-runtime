// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_TYPE_OPS_HPP
#define CEL_RUNTIME_TYPE_OPS_HPP

#include <cel/runtime/value_type.hpp>

namespace cel::detail {

/// Locates one ownership-carrying slot in canonical fixed-aggregate bytes.
struct ownership_slot {
    const value_type* type; ///< Borrows the slot's complete descriptor.
    std::size_t offset;     ///< Gives its canonical byte offset from the containing value.
};
/// Supplies a complete ownership pair for an opaque byte-transferable callable carrier.
/// clone creates independent carrier ownership; explicit reference-semantic code may be shared.
/// Mutable aggregate payloads are never shared. destroy never allocates or throws.
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
    /// Reports shared complete immutable metadata identity without traversing the shape.
    static bool same_plan(const value_type& a, const value_type& b) noexcept {
        return a._plan == b._plan;
    }
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
    /// Complexity: O(distinct visited plan pairs and edges) expected time and auxiliary space.
    static bool compatible_operations(const value_type& expected, const value_type& actual);
    /// Compares metadata with memoized plan pairs and no native recursion.
    /// semantic selects complete identity rather than only ownership operation compatibility.
    /// Precondition: semantic is true or complete shapes already match.
    /// Complexity: O(distinct visited plan pairs and edges) expected time and auxiliary space.
    static bool compare_plans(const value_type& expected, const value_type& actual, bool semantic);
    /// Returns the number of direct owning carriers in fixed canonical bytes.
    /// Arrays/callables count as one carrier; their separately owned children are not counted.
    static std::size_t resources(const value_type& type) noexcept;
    /// Returns the cached built-in/composite callable-parameter predicate.
    static bool parameter_allowed(const value_type& type) noexcept;
    /// Locates an ownership slot by ordinal without recursion or allocation.
    /// Precondition: ordinal < resources(type).
    /// Complexity: O(sum of log(field count + 1) along the selected path) time and O(1) space.
    static ownership_slot resource(const value_type& type, std::size_t ordinal) noexcept;
};

/// Validates complete type-level cloning and destruction capabilities.
/// Throws std::invalid_argument when any required opaque ownership operation is missing.
/// Empty arrays can own values even when their descriptor fails this conservative check.
void require_ownership(const value_type& type);

/// Validates complete shape and compatible opaque carrier ownership operations.
/// Semantic identity alone never authorizes using one opaque representation's operations
/// on another. Throws std::invalid_argument for shape or installed-operation mismatch.
/// Complexity: O(distinct visited plan pairs and edges) expected time and auxiliary space.
void require_compatible(const value_type& expected, const value_type& actual);

} // namespace cel::detail
#endif
