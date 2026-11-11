// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_VALUE_TYPE_HPP
#define CEL_RUNTIME_VALUE_TYPE_HPP

#include <cel/runtime/layout.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <typeindex>

namespace cel {

/// Identifies every source range form independently of its endpoint layout.
/// Example: `range_kind::range_inclusive` describes `start..=end`.
enum class range_kind {
    range,              ///< Describes `start..end`.
    range_from,         ///< Describes `start..`.
    range_to,           ///< Describes `..end`.
    range_to_inclusive, ///< Describes `..=end`.
    range_inclusive,    ///< Describes `start..=end`.
    range_full          ///< Describes `..`, retaining its scalar endpoint type.
};

/// Identifies a descriptor's structural category.
/// Example: an empty source unit has `type_kind::tuple`.
enum class type_kind {
    leaf,    ///< Stores a concrete trivially copyable client or built-in leaf.
    array,   ///< Owns a sequence of one element type.
    tuple,   ///< Stores ordered, canonically laid-out fields.
    record,  ///< Stores named fields and a nominal identity.
    range,   ///< Stores a range form and scalar endpoint type.
    callable ///< Retains parameters and a complete result signature.
};

struct record_field;
namespace detail {
struct type_plan;
struct type_access;
} // namespace detail

/// Shares an immutable, complete semantic type and canonical byte layout.
/// Leaf identity is concrete C++ type identity; records additionally retain their nominal
/// identity and field names. Capabilities do not change semantic identity.
/// Copies share immutable metadata, never mutable value payloads.
/// Borrowed references, spans, and string views remain valid while a descriptor retaining
/// their metadata remains alive. Keep this descriptor unchanged while using its views.
/// Factories retain component descriptors and copy supplied names; input spans and strings
/// need not outlive construction. See the example for value for record construction.
/// Example: `auto numbers = value_type::array(value_type::leaf<double>());`.
class value_type {
    friend struct detail::type_access;
    std::shared_ptr<const detail::type_plan> _plan;

    /// Takes exclusive construction metadata and publishes it as immutable.
    explicit value_type(std::shared_ptr<const detail::type_plan> plan);
    /// Registers a trusted concrete leaf with a checked layout.
    static value_type make_leaf(std::type_index identity, layout bytes);
    /// Releases uniquely owned metadata through node-owned worklists without allocating.
    /// Complexity: O(released plans + edges + name bytes) time and O(1) auxiliary space.
    static void release_plan(std::shared_ptr<const detail::type_plan>& plan) noexcept;

public:
    /// Creates the empty tuple descriptor (source unit).
    value_type();
    /// Shares the source's immutable metadata without changing the source.
    value_type(const value_type&) noexcept = default;
    /// Shares the source's immutable metadata, leaving its shape and capabilities unchanged.
    value_type(value_type&& other) noexcept : _plan(other._plan) {}
    /// Replaces this descriptor with the source's shape and capabilities; self-assignment is valid.
    /// Leaves the source unchanged and releases prior unique metadata without allocating.
    /// Complexity: O(released plans + edges + name bytes) time and O(1) auxiliary space.
    value_type& operator=(const value_type& other) noexcept;
    /// Shares the source's shape and capabilities and releases this descriptor's prior metadata.
    /// Leaves the source unchanged; self-move leaves this descriptor unchanged.
    /// Complexity: O(released prior plans + edges + name bytes) time and O(1) auxiliary space.
    value_type& operator=(value_type&& other) noexcept {
        return *this = static_cast<const value_type&>(other);
    }
    /// Releases this reference and uniquely owned descendants without recursion or allocation.
    /// Complexity: O(released plans + edges + name bytes) time and O(1) auxiliary space.
    ~value_type();

    /// Returns T's descriptor as an opaque, trivially copyable leaf.
    /// Rejects nontrivial T at compile time.
    /// This does not infer equality or ownership of objects referenced by T.
    /// Example: `auto boolean = value_type::leaf<bool>();`.
    template <class T>
    static value_type leaf() {
        static_assert(std::is_trivially_copyable_v<T>,
                      "opaque CEL leaves must be trivially copyable");
        static const value_type descriptor = make_leaf(typeid(T), {sizeof(T), alignof(T)});
        return descriptor;
    }
    /// Creates an array descriptor retaining the element's complete shape and capabilities.
    /// Reports owns_values() exactly when element does; constructing a type creates no values.
    /// Example: `auto type = value_type::array(value_type::leaf<bool>());`.
    static value_type array(value_type element);
    /// Creates a tuple descriptor with canonical source-order fields; empty fields describe unit.
    /// Retains every field's complete shape and ownership capabilities.
    /// Throws std::overflow_error if the canonical extent overflows.
    /// Complexity: O(field count) construction time and metadata space.
    /// Example: `auto unit = value_type::tuple({});`.
    static value_type tuple(std::span<const value_type> fields);
    /// Creates named fields with a nonempty nominal identity.
    /// Copies identity and field names, retaining each field's complete descriptor in source order.
    /// Throws std::invalid_argument for empty identity/names or duplicate field names.
    /// Throws std::overflow_error for canonical layout overflow.
    /// Complexity: O(field count + name bytes) expected time and metadata space.
    /// Example: `auto empty = value_type::record("sample", {});`.
    static value_type record(std::string_view identity, std::span<const record_field> fields);
    /// Creates a range with a concrete leaf endpoint and the specified source form.
    /// Throws std::invalid_argument for a non-leaf endpoint or invalid form.
    /// Throws std::overflow_error for canonical endpoint layout overflow.
    /// Bounded forms store two endpoints, one-sided forms store one, and full stores none.
    /// Example: `auto type = value_type::range(range_kind::range, value_type::leaf<double>());`.
    static value_type range(range_kind form, value_type endpoint);
    /// Creates a callable signature with bool/double parameters or composites of those leaves.
    /// Retains any complete result descriptor, but installs no callable ownership operations.
    /// Use bind_runtime_ownership when emission needs an ownership-capable signature.
    /// Throws std::invalid_argument if a parameter contains a client leaf or callable.
    /// Complexity: O(parameter count) time and metadata space; predicates are cached per plan.
    /// Example: `auto signature = value_type::callable({}, value_type::leaf<bool>());`.
    static value_type callable(std::span<const value_type> parameters, value_type result);
    /// Returns true exactly when identities, shapes, source ordering, and canonical layouts match.
    /// Ignores ownership capabilities; use compatible_with to admit stored representations.
    /// Complexity: O(distinct visited plan pairs, their edges, and name bytes) expected time
    /// and O(distinct visited plan pairs) auxiliary space; shared DAGs are not expanded.
    /// Example: `value_type::leaf<bool>().same_shape(value_type::leaf<double>()) == false`.
    bool same_shape(const value_type& other) const;
    /// Compares complete semantic shapes and compatible stored opaque ownership operations.
    /// Returns false for any shape, layout, identity, or stored-operation mismatch.
    /// Semantic comparison precedes ownership comparison; same_shape alone is insufficient.
    /// Callable compatibility includes the ownership of every nested result component.
    /// Compatibility does not prove installed ownership or initialized borrowed canonical data.
    /// Comparison can allocate and propagates allocation failures.
    /// Complexity: O(distinct visited plan pairs, their edges, and name bytes) expected time
    /// and O(distinct visited plan pairs) auxiliary space; shared DAGs are not expanded.
    /// Example: `value_type::leaf<double>().compatible_with(value_type::leaf<bool>()) == false`.
    bool compatible_with(const value_type& other) const;
    /// Returns the structural category.
    /// Example: `value_type{}.kind() == type_kind::tuple`.
    type_kind kind() const noexcept;
    /// Returns the canonical byte extent and alignment.
    /// Example: `value_type{}.byte_layout().size == 0`.
    layout byte_layout() const noexcept;
    /// Returns whether clone/destruction capabilities exist for every inhabited component.
    /// This describes capabilities, not the presence of initialized bytes.
    /// Callable ownership is unavailable until code ownership is installed.
    /// Example: `value_type::leaf<double>().owns_values() == true`.
    bool owns_values() const noexcept;
    /// Returns concrete leaf identity; throws std::invalid_argument for non-leaves.
    /// Example: `value_type::leaf<bool>().leaf_identity() == typeid(bool)`.
    std::type_index leaf_identity() const;
    /// Borrows the array element or range endpoint; throws std::invalid_argument otherwise.
    /// Example: `value_type::array(value_type::leaf<bool>()).element().kind() == type_kind::leaf`.
    const value_type& element() const;
    /// Borrows ordered tuple/record fields; throws std::invalid_argument otherwise.
    /// Example: `value_type{}.fields().empty() == true`.
    std::span<const value_type> fields() const;
    /// Borrows canonical tuple/record/range offsets; throws std::invalid_argument otherwise.
    /// Example: `value_type{}.offsets().empty() == true`.
    std::span<const std::size_t> offsets() const;
    /// Borrows nominal record identity; throws std::invalid_argument otherwise.
    /// Example: `value_type::record("sample", {}).record_identity() == "sample"`.
    std::string_view record_identity() const;
    /// Borrows source-order record names; throws std::invalid_argument otherwise.
    /// Example: `value_type::record("sample", {}).field_names().empty() == true`.
    std::span<const std::string> field_names() const;
    /// Returns the range form; throws std::invalid_argument otherwise.
    /// Example: `value_type::range(range_kind::range_full, value_type::leaf<bool>()).form()`.
    range_kind form() const;
    /// Borrows callable parameters; throws std::invalid_argument otherwise.
    /// Example: `value_type::callable({}, value_type{}).parameters().empty() == true`.
    std::span<const value_type> parameters() const;
    /// Borrows the complete callable result; throws std::invalid_argument otherwise.
    /// Example: `value_type::callable({}, value_type{}).result().kind() == type_kind::tuple`.
    const value_type& result() const;
};

/// Supplies a source-order name/type pair to value_type::record, which validates its name.
/// Example: `record_field{"enabled", value_type::leaf<bool>()}`.
struct record_field {
    std::string name; ///< Supplies a name; record construction rejects empty or duplicate names.
    value_type type;  ///< Retains the complete field descriptor.
};

/// Returns a runtime ownership-capable witness for the complete immutable type graph.
/// Preserves semantic identity, layout, names, ordering, parameter admission, and results.
/// Binds deferred callables, including nested results, to the runtime's canonical carrier
/// operations; retains already installed canonical ownership and shares unchanged metadata.
/// The input remains unchanged. Binding creates no live callable, body, or initialized bytes
/// and invokes no ownership operations. Repeated binding is representation-compatible.
/// Throws std::invalid_argument for foreign or incompatible installed callable operations
/// anywhere in the graph. Allocation failures propagate without changing the input.
/// Complexity: expected O(V + E + S) time and auxiliary space for V distinct metadata nodes,
/// E edges, and S copied name bytes; traversal and cleanup use no native recursion.
/// Example (with `<cel/runtime.hpp>`):
/// @code
/// auto fn = cel::bind_runtime_ownership(
///     cel::value_type::callable({}, cel::value_type::leaf<double>()));
/// cel::dyn_segment builder;
/// builder.make_array(0, fn, {});
/// @endcode
value_type bind_runtime_ownership(const value_type& type);

} // namespace cel
#endif
