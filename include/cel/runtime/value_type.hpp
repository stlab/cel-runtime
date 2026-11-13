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
/// \addtogroup runtime_values
/// @{

/// Identifies every source range form independently of its endpoint layout.
///
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
///
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

/// An immutable, shared semantic type with a canonical byte layout.
///
/// Leaf identity is concrete C++ type identity; records additionally retain their nominal
/// identity and field names. Capabilities do not change semantic identity.
/// Copies share immutable metadata, never mutable value payloads.
/// Borrowed references, spans, and string views remain valid while a descriptor retaining
/// their metadata remains alive. Keep this descriptor unchanged while using its views.
/// Access to one descriptor must not race its replacement or destruction.
/// Factories retain component descriptors and copy supplied names; input spans and strings
/// need not outlive construction.
///
/// See value's shared example for record construction and descriptor copying.
/// Example: `auto numbers = value_type::array(value_type::leaf<double>()); auto copy = numbers;`.
class value_type {
    friend struct detail::type_access;
    std::shared_ptr<const detail::type_plan> _plan;

    /// Retains complete immutable construction metadata.
    ///
    /// \pre The plan is complete and no mutable alias can change it after publication.
    explicit value_type(std::shared_ptr<const detail::type_plan> plan);
    /// Registers a trusted concrete leaf with a checked layout.
    ///
    /// \pre Identity and layout describe the same trivially copyable concrete type.
    /// Throws std::invalid_argument for invalid alignment and std::overflow_error for
    /// alignment rounding overflow.
    static value_type make_leaf(std::type_index identity, layout bytes);
    /// Releases this reference and uniquely owned metadata without recursion or allocation.
    ///
    /// Complexity: O(V + E + S) time and O(1) auxiliary space, for released plans V,
    /// their descriptor edges E, and stored name bytes S.
    static void release_plan(std::shared_ptr<const detail::type_plan>& plan) noexcept;

public:
    /// Creates the empty tuple descriptor (source unit).
    ///
    /// See the tuple example.
    value_type();
    /// Shares the source's immutable metadata without changing the source.
    ///
    /// See the class's shared copying example.
    value_type(const value_type&) noexcept = default;
    /// Shares the source's immutable metadata, leaving its shape and capabilities unchanged.
    ///
    /// Example: `auto copy = std::move(type); // type retains its descriptor.`
    value_type(value_type&& other) noexcept : _plan(other._plan) {}
    /// Replaces this descriptor with the source's shape and capabilities; self-assignment is valid.
    ///
    /// Leaves the source unchanged and releases prior unique metadata without allocating.
    /// Complexity: O(released plans + edges + name bytes) time and O(1) auxiliary space.
    ///
    /// Example: `auto type = value_type{}; type = value_type::leaf<bool>();`.
    value_type& operator=(const value_type& other) noexcept;
    /// Shares the source's shape and capabilities and releases this descriptor's prior metadata.
    ///
    /// Leaves the source unchanged; self-move leaves this descriptor unchanged.
    /// Complexity: O(released prior plans + edges + name bytes) time and O(1) auxiliary space.
    ///
    /// Example: `auto type = value_type{}; type = std::move(other); // other is unchanged.`
    value_type& operator=(value_type&& other) noexcept {
        return *this = static_cast<const value_type&>(other);
    }
    /// Releases this reference and uniquely owned descendants without recursion or allocation.
    ///
    /// Complexity: O(released plans + edges + name bytes) time and O(1) auxiliary space.
    ///
    /// See the class's shared copying example.
    ~value_type();

    /// Returns T's descriptor as an opaque, trivially copyable leaf.
    ///
    /// This does not infer equality or ownership of objects referenced by T.
    ///
    /// Example: `auto boolean = value_type::leaf<bool>();`.
    template <class T>
    static value_type leaf() {
        static_assert(std::is_trivially_copyable_v<T>,
                      "opaque CEL leaves must be trivially copyable");
        static const value_type descriptor = make_leaf(typeid(T), {sizeof(T), alignof(T)});
        return descriptor;
    }
    /// Creates an array descriptor retaining the element's complete shape and capabilities.
    ///
    /// Reports owns_values() exactly when element does; constructing a type creates no values.
    ///
    /// Example: `auto type = value_type::array(value_type::leaf<bool>());`.
    static value_type array(value_type element);
    /// Creates a tuple descriptor with canonical source-order fields; empty fields describe unit.
    ///
    /// Retains every field's complete shape and ownership capabilities.
    ///
    /// Throws std::overflow_error if the canonical extent overflows.
    /// Complexity: O(field count) construction time and metadata space.
    ///
    /// Example: `auto unit = value_type::tuple({});`.
    static value_type tuple(std::span<const value_type> fields);
    /// Creates named fields with a nonempty nominal identity.
    ///
    /// Copies identity and field names, retaining each field's complete descriptor in source order.
    ///
    /// Throws std::invalid_argument for empty identity/names or duplicate field names.
    /// Throws std::overflow_error for canonical layout overflow.
    /// Complexity: O(field count + identity/name bytes) expected time and result metadata
    /// space; O(field count) auxiliary space for name validation.
    ///
    /// Example: `auto empty = value_type::record("sample", {});`.
    static value_type record(std::string_view identity, std::span<const record_field> fields);
    /// Creates a range with a concrete leaf endpoint and the specified source form.
    ///
    /// Bounded forms store two endpoints, one-sided forms store one, and full stores none.
    ///
    /// Throws std::invalid_argument for a non-leaf endpoint or invalid form.
    /// Throws std::overflow_error for canonical endpoint layout overflow.
    ///
    /// Example: `auto type = value_type::range(range_kind::range, value_type::leaf<double>());`.
    static value_type range(range_kind form, value_type endpoint);
    /// Creates a callable signature with bool/double parameters or composites of those leaves.
    ///
    /// Retains any complete result descriptor, but installs no callable ownership operations.
    /// Use bind_runtime_ownership when emission needs an ownership-capable signature.
    ///
    /// Throws std::invalid_argument if a parameter contains a client leaf or callable.
    /// Complexity: O(parameter count) time and result metadata space.
    ///
    /// Example: `auto signature = value_type::callable({}, value_type::leaf<bool>());`.
    static value_type callable(std::span<const value_type> parameters, value_type result);
    /// Returns true exactly when identities, shapes, source ordering, and canonical layouts match.
    ///
    /// Ignores ownership capabilities; use compatible_with to admit stored representations.
    /// Allocation failures leave both descriptors unchanged.
    ///
    /// Complexity: O(P + E + S) expected time and O(P + E) auxiliary space, for distinct
    /// visited plan pairs P, their edges E, and compared identity/name bytes S.
    /// Shared subgraphs are not expanded recursively; identical plans compare in O(1).
    ///
    /// Example: `value_type::leaf<bool>().same_shape(value_type::leaf<double>()) == false`.
    bool same_shape(const value_type& other) const;
    /// Compares complete semantic shapes and compatible stored opaque ownership operations.
    ///
    /// Returns false for any shape, layout, identity, or stored-operation mismatch.
    /// Semantic comparison precedes ownership comparison; same_shape alone is insufficient.
    /// Callable compatibility includes the ownership of every nested result component.
    /// Compatibility does not prove installed ownership or initialized borrowed canonical data.
    ///
    /// Allocation failures leave both descriptors unchanged.
    ///
    /// Complexity: O(P + E + S) expected time and O(P + E) auxiliary space, with
    /// P, E, and S as in same_shape; identical plans compare in O(1).
    ///
    /// Example: `value_type::leaf<double>().compatible_with(value_type::leaf<bool>()) == false`.
    bool compatible_with(const value_type& other) const;
    /// Returns the structural category.
    ///
    /// Example: `value_type{}.kind() == type_kind::tuple`.
    type_kind kind() const noexcept;
    /// Returns the canonical byte extent and alignment.
    ///
    /// Example: `value_type{}.byte_layout().size == 0`.
    layout byte_layout() const noexcept;
    /// Returns whether clone/destruction capabilities exist for every inhabited component.
    ///
    /// This describes capabilities, not the presence of initialized bytes.
    /// Callable ownership is unavailable until code ownership is installed.
    ///
    /// Example: `value_type::leaf<double>().owns_values() == true`.
    bool owns_values() const noexcept;
    /// Returns concrete leaf identity.
    ///
    /// Throws std::invalid_argument for non-leaves.
    ///
    /// Example: `value_type::leaf<bool>().leaf_identity() == typeid(bool)`.
    std::type_index leaf_identity() const;
    /// Borrows this descriptor's array element or range endpoint.
    ///
    /// Throws std::invalid_argument for other categories.
    ///
    /// Example: `value_type::array(value_type::leaf<bool>()).element().kind() == type_kind::leaf`.
    const value_type& element() const;
    /// Borrows this descriptor's ordered tuple/record fields.
    ///
    /// Throws std::invalid_argument for other categories.
    ///
    /// Example: `value_type{}.fields().empty() == true`.
    std::span<const value_type> fields() const;
    /// Borrows this descriptor's canonical tuple/record/range offsets.
    ///
    /// Throws std::invalid_argument for other categories.
    ///
    /// Example: `value_type{}.offsets().empty() == true`.
    std::span<const std::size_t> offsets() const;
    /// Borrows this descriptor's nominal record identity.
    ///
    /// Throws std::invalid_argument for non-records.
    ///
    /// Example: `value_type::record("sample", {}).record_identity() == "sample"`.
    std::string_view record_identity() const;
    /// Borrows this descriptor's source-order record names.
    ///
    /// Throws std::invalid_argument for non-records.
    ///
    /// Example: `value_type::record("sample", {}).field_names().empty() == true`.
    std::span<const std::string> field_names() const;
    /// Returns the range form.
    ///
    /// Throws std::invalid_argument for non-ranges.
    ///
    /// Example: `value_type::range(range_kind::range_full, value_type::leaf<bool>()).form()`.
    range_kind form() const;
    /// Borrows this descriptor's callable parameters.
    ///
    /// Throws std::invalid_argument for non-callables.
    ///
    /// Example: `value_type::callable({}, value_type{}).parameters().empty() == true`.
    std::span<const value_type> parameters() const;
    /// Borrows this descriptor's complete callable result.
    ///
    /// Throws std::invalid_argument for non-callables.
    ///
    /// Example: `value_type::callable({}, value_type{}).result().kind() == type_kind::tuple`.
    const value_type& result() const;
};

/// A source-order name/type pair validated by value_type::record.
///
/// Example: `record_field{"enabled", value_type::leaf<bool>()}`.
struct record_field {
    std::string name; ///< Supplies a name; record construction rejects empty or duplicate names.
    value_type type;  ///< Retains the complete field descriptor.
};

/// Returns a runtime ownership-capable witness for the complete immutable type graph.
///
/// Preserves semantic identity, layout, names, ordering, parameter admission, and results.
/// Binds deferred callables, including nested results, to the runtime's canonical carrier
/// operations; retains already installed canonical ownership and shares unchanged metadata.
/// The input remains unchanged. Binding creates no live callable, body, or initialized bytes
/// and invokes no ownership operations. Repeated binding is representation-compatible.
///
/// Throws std::invalid_argument for foreign or incompatible installed callable operations
/// anywhere in the graph. Allocation failures propagate without changing the input.
///
/// Complexity: expected O(V + E + S) time, O(V) auxiliary space, and
/// O(V + E + S) new result metadata for distinct nodes V, edges E, and copied
/// identity/name bytes S. Traversal and cleanup use no native recursion.
///
/// Example (with `<cel/runtime.hpp>`):
/// @code
/// auto fn = cel::bind_runtime_ownership(
///     cel::value_type::callable({}, cel::value_type::leaf<double>()));
/// cel::dyn_segment builder;
/// builder.make_array(0, fn, {});
/// @endcode
value_type bind_runtime_ownership(const value_type& type);

/// @}
} // namespace cel
#endif
