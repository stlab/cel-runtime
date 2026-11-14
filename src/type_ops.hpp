// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_TYPE_OPS_HPP
#define CEL_RUNTIME_TYPE_OPS_HPP

#include <array>
#include <cel/runtime/value_type.hpp>
#include <limits>
#include <optional>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cel::detail {

/// A borrowed carrier or reusable ownership subrange in canonical fixed-aggregate bytes.
///
/// The containing descriptor retains type; its views must remain valid while the slot is used.
struct ownership_slot {
    const value_type* type; ///< Borrows the slot's complete descriptor.
    std::size_t offset;     ///< Gives its canonical byte offset from the containing value.
};
/// A complete ownership pair for an opaque byte-transferable callable carrier.
///
/// clone creates independent carrier ownership; explicit reference-semantic code may be shared.
/// Mutable aggregate payloads are never shared. destroy never allocates or throws.
/// A throwing clone must release its partial construction and leave destination uninitialized.
/// Both functions operate on the signature's canonical extent; no operand move callback exists.
/// Caller-provided operations retain their own time and auxiliary-space costs.
/// Calls require exclusive access to the affected carrier; sharing code is not synchronization.
struct opaque_ownership {
    /// Clones one carrier under this ownership pair's copy semantics.
    ///
    /// \pre Source is a readable valid carrier; destination is disjoint aligned
    /// uninitialized writable storage of the signature's canonical extent.
    /// Failure releases partial construction and leaves destination uninitialized.
    void (*clone)(const std::byte*, std::byte*) = nullptr;
    /// Destroys one carrier without allocating.
    ///
    /// \pre The argument is an exclusively accessible valid owned carrier of this
    /// signature; a transferred source is not destroyed.
    void (*destroy)(std::byte*) noexcept = nullptr;
};
static_assert(std::is_trivially_copyable_v<opaque_ownership>);

/// Hashes borrowed immutable descriptor identity pairs.
struct plan_pair_hash {
    /// Combines the two metadata identities.
    std::size_t operator()(
        const std::pair<const type_plan*, const type_plan*>& pair) const noexcept;
};

/// Memoizes successful shape and ownership checks for one checked operation.
///
/// Retains compared descriptors, including consumed inputs, until the operation ends.
/// Shape and ownership successes are independent; failed comparisons are never cached.
/// For an operation ending at its first mismatch, complexity is expected O(P + E + S)
/// total comparison time and O(P + E) auxiliary space for distinct compared pairs P,
/// their edges E, and compared name bytes S.
class compatibility_context {
    friend struct type_access;
    using pair = std::pair<const type_plan*, const type_plan*>;
    std::unordered_set<pair, plan_pair_hash> _semantic;
    std::unordered_set<pair, plan_pair_hash> _operations;
    std::vector<value_type> _retained;
    std::size_t _compared = 0;

public:
    /// Starts an empty operation-local memo without allocating.
    compatibility_context() = default;
    /// Checks complete shapes and ownership operations and returns the newly compared pair count.
    ///
    /// Reuses successful pair checks; shape and operation comparisons count separately.
    /// Throws std::invalid_argument on mismatch, leaving inputs unchanged.
    /// Failed checks do not authorize a later comparison.
    std::size_t require(const value_type& expected, const value_type& actual);
};

/// Trusted access to immutable type identity and ownership capabilities.
///
/// These operations do not implement callable execution or concrete closure bodies.
struct type_access {
    /// Reports shared complete immutable metadata identity without traversing the shape.
    static bool same_plan(const value_type& a, const value_type& b) noexcept {
        return a._plan == b._plan;
    }
    /// Returns the signature with a complete installed ownership pair.
    ///
    /// Throws std::invalid_argument for non-callables or missing operations.
    /// \pre Nonnull supplied operations satisfy opaque_ownership's semantic and safety contract.
    /// Preserves signature identity and leaves the input unchanged, including on failure.
    /// Complexity: O(parameter count) time and result metadata space.
    static value_type install_callable_ownership(const value_type& signature,
                                                 opaque_ownership operations);
    /// Binds every deferred callable in an immutable metadata DAG to the supplied canonical pair.
    ///
    /// \pre canonical supplies the runtime's complete clone/destruction pair.
    /// Rejects foreign installed operations with std::invalid_argument without invoking them.
    /// Preserves source metadata and parameter/layout/resource invariants; shares unchanged nodes.
    /// Allocation failures propagate; partial metadata is safely released without recursion.
    /// Complexity: expected O(V + E + S) time, O(V) auxiliary space, and
    /// O(V + E + S) result metadata, for distinct nodes V, edges E, and copied
    /// identity/name bytes S.
    static value_type bind_runtime_ownership(const value_type& type, opaque_ownership canonical);
    /// Returns the installed pair, or an empty pair for a deferred callable.
    ///
    /// \pre type is callable.
    static opaque_ownership operations(const value_type& type) noexcept;
    /// Compares installed carrier operations, short-circuiting shared immutable plans.
    ///
    /// \pre Complete semantic shapes already match.
    /// Includes callable results and nested result components; leaves inputs unchanged on failure.
    /// Complexity: O(P + E) expected time and auxiliary space for distinct visited
    /// plan pairs P and their edges E.
    static bool compatible_operations(const value_type& expected, const value_type& actual);
    /// Compares complete semantic identity or installed ownership-operation compatibility.
    ///
    /// semantic selects identity; false selects ownership operations, including nested results.
    /// \pre semantic is true or complete shapes already match.
    /// Allocation failures leave both inputs unchanged; traversal uses no native recursion.
    /// Complexity: O(P + E + S) expected time and O(P + E) auxiliary space for distinct
    /// visited plan pairs P, their edges E, and compared identity/name bytes S.
    /// S is zero for operation-only comparisons; identical plans compare in O(1).
    static bool compare_plans(const value_type& expected, const value_type& actual, bool semantic);
    /// Compares plans using an operation's successful checks for the selected comparison.
    ///
    /// \pre For operation-only comparison, complete semantic shapes already match.
    /// Follows compare_plans's contract and retains any newly compared descriptors.
    static bool compare_plans(const value_type& expected,
                              const value_type& actual,
                              bool semantic,
                              compatibility_context& context);
    /// Returns the number of direct owning carriers in fixed canonical bytes.
    ///
    /// Arrays/callables count as one carrier; their separately owned children are not counted.
    static std::size_t resources(const value_type& type) noexcept;
    /// Returns whether type is bool/double or a composite of admitted parameter types.
    ///
    /// Client leaves and callables are not admitted, including inside empty arrays/full ranges.
    static bool parameter_allowed(const value_type& type) noexcept;
    /// Borrows the compiled ownership sequence's immediate reusable ranges.
    ///
    /// Scalars and carriers have no subranges. Single-range paths are collapsed;
    /// branching sequences put a largest carrier-count range last.
    /// The view remains valid while type's metadata is retained.
    static std::span<const ownership_slot> ownership_ranges(const value_type& type) noexcept;
};

/// Scans a compiled ownership sequence without allocation or nesting-dependent call space.
///
/// Borrows the source descriptor for its lifetime. Each next() performs one constant-time
/// transition; the complete scan takes O(1 + carrier count) time and O(1) auxiliary space.
class ownership_cursor {
    /// A borrowed sequence position and its byte offset from the traversal root.
    struct position {
        const value_type* type;
        std::size_t offset;
        std::size_t next = 0;
    };
    // Every pending non-final descent halves the carrier count. Final descents replace
    // their frame, so machine-word width bounds the stack even for skewed descriptors.
    std::array<position, std::numeric_limits<std::size_t>::digits + 1> _pending;
    std::size_t _size = 0;

public:
    /// Starts a traversal of type's direct ownership carriers.
    explicit ownership_cursor(const value_type& type) noexcept;
    /// Reports whether every carrier has been emitted.
    bool empty() const noexcept { return _size == 0; }
    /// Advances one traversal transition, returning a slot only when it reaches a carrier.
    ///
    /// \pre The cursor is not empty.
    std::optional<ownership_slot> next() noexcept;
};

/// Validates complete type-level cloning and destruction capabilities.
///
/// Throws std::invalid_argument when any required opaque ownership operation is missing.
/// Empty arrays can own values even when their descriptor fails this conservative check.
void require_ownership(const value_type& type);

/// Validates complete shape and compatible opaque carrier ownership operations.
///
/// Semantic identity alone never authorizes using one opaque representation's operations
/// on another. Throws std::invalid_argument for shape or installed-operation mismatch.
/// Allocation failures also leave both inputs unchanged.
/// Complexity: O(P + E + S) expected time and O(P + E) auxiliary space, with
/// P, E, and S as in value_type::same_shape.
void require_compatible(const value_type& expected, const value_type& actual);

} // namespace cel::detail
#endif
