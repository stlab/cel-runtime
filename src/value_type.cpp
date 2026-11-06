// SPDX-License-Identifier: BSL-1.0
#include "type_ops.hpp"
#include <cel/runtime/value_type.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <vector>

namespace cel::detail {

static_assert(sizeof(double) == 8 && std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53,
              "CEL built-in double requires IEEE binary64");
void require_ownership(const value_type& type) {
    if (!type.owns_values())
        throw std::invalid_argument("complete ownership operations unavailable");
}

/// Owns immutable descriptor metadata after construction.
/// Invariant: all fields and offsets are canonical and category-appropriate.
struct type_plan {
    type_kind kind = type_kind::tuple;        ///< Identifies the shape.
    layout bytes{0, 1};                       ///< Describes its canonical representation.
    bool owns = true;                         ///< Records complete ownership capabilities.
    std::type_index leaf{typeid(void)};       ///< Identifies a concrete leaf.
    std::vector<value_type> fields;           ///< Owns ordered fields or callable parameters.
    std::vector<std::size_t> offsets;         ///< Owns ordered aggregate offsets.
    std::vector<std::string> names;           ///< Owns record field names.
    std::string identity;                     ///< Owns nominal record identity.
    std::optional<value_type> element;        ///< Retains array/range element identity.
    std::optional<value_type> result;         ///< Retains a complete callable result.
    range_kind form = range_kind::range_full; ///< Retains source range form.
    opaque_ownership operations; ///< Retains trusted callable clone/destruction operations.
};

value_type type_access::install_callable_ownership(const value_type& signature,
                                                   opaque_ownership operations) {
    if (signature.kind() != type_kind::callable || !operations.clone || !operations.destroy)
        throw std::invalid_argument("callable ownership requires both operations");
    auto plan = std::make_shared<type_plan>(*signature._plan);
    plan->operations = operations;
    plan->owns = true;
    return value_type(std::move(plan));
}

opaque_ownership type_access::operations(const value_type& type) noexcept {
    assert(type.kind() == type_kind::callable);
    return type._plan->operations;
}

bool type_access::compatible_operations(const value_type& expected, const value_type& actual) {
    if (expected._plan == actual._plan) return true;
    switch (expected.kind()) {
        case type_kind::leaf:
        case type_kind::range:
            return true;
        case type_kind::array:
            return compatible_operations(expected.element(), actual.element());
        case type_kind::callable: {
            const auto a = type_access::operations(expected);
            const auto b = type_access::operations(actual);
            return a.clone == b.clone && a.destroy == b.destroy;
        }
        case type_kind::tuple:
        case type_kind::record:
            return std::equal(expected.fields().begin(), expected.fields().end(),
                              actual.fields().begin(), compatible_operations);
    }
    return false;
}

void require_compatible(const value_type& expected, const value_type& actual) {
    if (!expected.same_shape(actual) || !type_access::compatible_operations(expected, actual))
        throw std::invalid_argument("incompatible value shape or opaque ownership operations");
}

/// Computes canonical offsets, tail padding, and aggregate ownership capabilities.
/// Throws std::overflow_error if byte extent arithmetic overflows.
/// Complexity: O(field count) time and offset space.
void canonical_layout(type_plan& plan, std::span<const value_type> fields) {
    // Accumulate source-order layouts and retain canonical field offsets.
    for (const auto& field : fields) {
        const auto bytes = field.byte_layout();
        const auto offset = align_offset(plan.bytes.size, bytes.alignment);
        if (bytes.size > std::numeric_limits<std::size_t>::max() - offset)
            throw std::overflow_error("aggregate layout overflow");
        plan.offsets.push_back(offset);
        plan.bytes.size = offset + bytes.size;
        plan.bytes.alignment = std::max(plan.bytes.alignment, bytes.alignment);
        plan.owns = plan.owns && field.owns_values();
    }
    plan.bytes.size = align_offset(plan.bytes.size, plan.bytes.alignment);
}

/// Reports whether a parameter contains only built-in leaves and composite shapes.
/// Complexity: O(shape size) time and O(shape depth) call space.
bool parameter_allowed(const value_type& type) {
    switch (type.kind()) {
        case type_kind::leaf:
            return type.leaf_identity() == typeid(bool) || type.leaf_identity() == typeid(double);
        case type_kind::array:
        case type_kind::range:
            return parameter_allowed(type.element());
        case type_kind::tuple:
        case type_kind::record:
            return std::all_of(type.fields().begin(), type.fields().end(), parameter_allowed);
        case type_kind::callable:
            return false;
    }
    return false;
}

} // namespace cel::detail

namespace cel {

value_type::value_type(std::shared_ptr<const detail::type_plan> plan) : _plan(std::move(plan)) {}
value_type::value_type() : value_type(tuple({})) {}

value_type value_type::make_leaf(std::type_index identity, layout bytes) {
    (void)align_offset(bytes.size, bytes.alignment);
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::leaf;
    plan->bytes = bytes;
    plan->leaf = identity;
    return value_type(std::move(plan));
}

value_type value_type::array(value_type element) {
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::array;
    plan->bytes = {sizeof(void*), alignof(void*)};
    plan->owns = element.owns_values();
    plan->element = std::move(element);
    return value_type(std::move(plan));
}

value_type value_type::tuple(std::span<const value_type> fields) {
    auto plan = std::make_shared<detail::type_plan>();
    plan->fields.assign(fields.begin(), fields.end());
    detail::canonical_layout(*plan, fields);
    return value_type(std::move(plan));
}

value_type value_type::record(std::string_view identity, std::span<const record_field> fields) {
    if (identity.empty()) throw std::invalid_argument("record identity is empty");
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::record;
    plan->identity = identity;
    std::unordered_set<std::string_view> names;
    // Validate and retain source-order names and complete component types.
    for (const auto& field : fields) {
        if (field.name.empty() || !names.insert(field.name).second)
            throw std::invalid_argument("record field name is empty or duplicated");
        plan->names.push_back(field.name);
        plan->fields.push_back(field.type);
    }
    detail::canonical_layout(*plan, plan->fields);
    return value_type(std::move(plan));
}

value_type value_type::range(range_kind form, value_type endpoint) {
    if (endpoint.kind() != type_kind::leaf)
        throw std::invalid_argument("range endpoint must be a scalar leaf");
    std::size_t count = 0;
    switch (form) {
        case range_kind::range:
        case range_kind::range_inclusive:
            count = 2;
            break;
        case range_kind::range_from:
        case range_kind::range_to:
        case range_kind::range_to_inclusive:
            count = 1;
            break;
        case range_kind::range_full:
            break;
        default:
            throw std::invalid_argument("invalid range form");
    }
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::range;
    plan->form = form;
    plan->element = endpoint;
    const std::array endpoints{endpoint, endpoint};
    detail::canonical_layout(*plan, std::span(endpoints).first(count));
    return value_type(std::move(plan));
}

value_type value_type::callable(std::span<const value_type> parameters, value_type result) {
    if (!std::all_of(parameters.begin(), parameters.end(), detail::parameter_allowed))
        throw std::invalid_argument("callable parameters require built-in composite values");
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::callable;
    plan->bytes = {sizeof(void*), alignof(void*)};
    plan->owns = false;
    plan->fields.assign(parameters.begin(), parameters.end());
    plan->result = std::move(result);
    return value_type(std::move(plan));
}

bool value_type::same_shape(const value_type& other) const {
    if (_plan == other._plan) return true;
    if (_plan->kind != other._plan->kind || _plan->bytes.size != other._plan->bytes.size ||
        _plan->bytes.alignment != other._plan->bytes.alignment ||
        _plan->leaf != other._plan->leaf || _plan->identity != other._plan->identity ||
        _plan->names != other._plan->names || _plan->form != other._plan->form ||
        _plan->offsets != other._plan->offsets ||
        _plan->element.has_value() != other._plan->element.has_value() ||
        _plan->result.has_value() != other._plan->result.has_value())
        return false;
    if (_plan->element && !_plan->element->same_shape(*other._plan->element)) return false;
    if (_plan->result && !_plan->result->same_shape(*other._plan->result)) return false;
    return std::equal(_plan->fields.begin(), _plan->fields.end(), other._plan->fields.begin(),
                      other._plan->fields.end(),
                      [](const auto& a, const auto& b) { return a.same_shape(b); });
}

type_kind value_type::kind() const noexcept { return _plan->kind; }
layout value_type::byte_layout() const noexcept { return _plan->bytes; }
bool value_type::owns_values() const noexcept { return _plan->owns; }

std::type_index value_type::leaf_identity() const {
    if (kind() != type_kind::leaf) throw std::invalid_argument("not a concrete leaf");
    return _plan->leaf;
}
const value_type& value_type::element() const {
    if (!_plan->element) throw std::invalid_argument("not an array or range");
    return *_plan->element;
}
std::span<const value_type> value_type::fields() const {
    if (kind() != type_kind::tuple && kind() != type_kind::record)
        throw std::invalid_argument("not a tuple or record");
    return _plan->fields;
}
std::span<const std::size_t> value_type::offsets() const {
    if (kind() != type_kind::tuple && kind() != type_kind::record && kind() != type_kind::range)
        throw std::invalid_argument("not a fixed aggregate");
    return _plan->offsets;
}
std::string_view value_type::record_identity() const {
    if (kind() != type_kind::record) throw std::invalid_argument("not a record");
    return _plan->identity;
}
std::span<const std::string> value_type::field_names() const {
    if (kind() != type_kind::record) throw std::invalid_argument("not a record");
    return _plan->names;
}
range_kind value_type::form() const {
    if (kind() != type_kind::range) throw std::invalid_argument("not a range");
    return _plan->form;
}
std::span<const value_type> value_type::parameters() const {
    if (kind() != type_kind::callable) throw std::invalid_argument("not a callable");
    return _plan->fields;
}
const value_type& value_type::result() const {
    if (!_plan->result) throw std::invalid_argument("not a callable");
    return *_plan->result;
}

} // namespace cel
