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
/// Example: `auto numbers = value_type::array(value_type::leaf<double>());`.
class value_type {
    friend struct detail::type_access;
    std::shared_ptr<const detail::type_plan> _plan;

    /// Takes exclusive construction metadata and publishes it as immutable.
    explicit value_type(std::shared_ptr<const detail::type_plan> plan);
    /// Registers a trusted concrete leaf with a checked layout.
    static value_type make_leaf(std::type_index identity, layout bytes);

public:
    /// Creates the empty tuple descriptor (source unit).
    value_type();
    /// Shares immutable metadata.
    value_type(const value_type&) noexcept = default;
    /// Shares immutable metadata and leaves the source descriptor valid.
    value_type(value_type&& other) noexcept : _plan(other._plan) {}
    /// Shares immutable metadata, including on self-assignment.
    value_type& operator=(const value_type&) noexcept = default;
    /// Shares immutable metadata and leaves the source descriptor valid.
    value_type& operator=(value_type&& other) noexcept {
        _plan = other._plan;
        return *this;
    }
    /// Releases this reference to the metadata.
    ~value_type() = default;

    /// Registers T as an opaque, trivially copyable leaf; rejects nontrivial T at compile time.
    /// This does not infer equality or ownership of objects referenced by T.
    /// Example: `auto boolean = value_type::leaf<bool>();`.
    template <class T>
    static value_type leaf() {
        static_assert(std::is_trivially_copyable_v<T>,
                      "opaque CEL leaves must be trivially copyable");
        static const value_type descriptor = make_leaf(typeid(T), {sizeof(T), alignof(T)});
        return descriptor;
    }
    /// Creates an owning array descriptor, retaining the complete element shape.
    /// Example: `auto type = value_type::array(value_type::leaf<bool>());`.
    static value_type array(value_type element);
    /// Creates canonical source-order fields; empty fields describe source unit.
    /// Throws std::overflow_error if the canonical extent overflows.
    /// Complexity: O(field count) construction time and metadata space.
    /// Example: `auto unit = value_type::tuple({});`.
    static value_type tuple(std::span<const value_type> fields);
    /// Creates named fields with a nonempty nominal identity.
    /// Throws std::invalid_argument for empty identity/names or duplicate field names.
    /// Throws std::overflow_error for canonical layout overflow.
    /// Complexity: O(field count + name bytes) expected time and metadata space.
    /// Example: `auto empty = value_type::record("sample", {});`.
    static value_type record(std::string_view identity, std::span<const record_field> fields);
    /// Creates a range with a concrete leaf endpoint and the specified source form.
    /// Throws std::invalid_argument for a non-leaf endpoint or invalid form.
    /// Example: `auto type = value_type::range(range_kind::range, value_type::leaf<double>());`.
    static value_type range(range_kind form, value_type endpoint);
    /// Creates a complete signature with built-in/composite parameters and any valid result.
    /// Throws std::invalid_argument if a parameter contains a client leaf or callable.
    /// Complexity: O(parameter shape size) time and O(parameter count) metadata space.
    /// Example: `auto signature = value_type::callable({}, value_type::leaf<bool>());`.
    static value_type callable(std::span<const value_type> parameters, value_type result);
    /// Compares complete identities, shapes, source ordering, and canonical layouts.
    /// Complexity: O(combined shape size and name bytes) time and O(shape depth) call space.
    /// Example: `value_type::leaf<bool>().same_shape(value_type::leaf<double>()) == false`.
    bool same_shape(const value_type& other) const;
    /// Returns the structural category.
    /// Example: `value_type{}.kind() == type_kind::tuple`.
    type_kind kind() const noexcept;
    /// Returns the canonical byte extent and alignment.
    /// Example: `value_type{}.byte_layout().size == 0`.
    layout byte_layout() const noexcept;
    /// Reports whether clone/destruction capabilities exist for every inhabited component.
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

/// Defines a named source-order record component.
/// Example: `record_field{"enabled", value_type::leaf<bool>()}`.
struct record_field {
    std::string name; ///< Stores a nonempty name unique within its record.
    value_type type;  ///< Retains the complete field descriptor.
};

} // namespace cel
#endif
