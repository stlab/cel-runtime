// SPDX-License-Identifier: BSL-1.0
#include "type_ops.hpp"
#include <cel/runtime/value_type.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <exception>
#include <limits>
#include <optional>
#include <stdexcept>
#include <unordered_map>
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
    bool parameter = true;                    ///< Caches built-in/composite parameter admission.
    std::size_t resources = 0;                ///< Counts direct ownership carriers.
    std::type_index leaf{typeid(void)};       ///< Identifies a concrete leaf.
    std::vector<value_type> fields;           ///< Owns ordered fields or callable parameters.
    std::vector<std::size_t> offsets;         ///< Owns ordered aggregate offsets.
    std::vector<std::size_t> resource_ends;   ///< Caches cumulative source-order carrier counts.
    std::vector<std::string> names;           ///< Owns record field names.
    std::string identity;                     ///< Owns nominal record identity.
    std::optional<value_type> element;        ///< Retains array/range element identity.
    std::optional<value_type> result;         ///< Retains a complete callable result.
    range_kind form = range_kind::range_full; ///< Retains source range form.
    opaque_ownership operations; ///< Retains trusted callable clone/destruction operations.
    std::shared_ptr<const type_plan> release_next; ///< Links only dying, uniquely owned plans.
};

/// Hashes a memoized metadata pair without depending on structural expansion.
struct plan_pair_hash {
    /// Combines pointer identities for immutable plan-pair lookup.
    std::size_t operator()(
        const std::pair<const type_plan*, const type_plan*>& pair) const noexcept {
        const auto a = std::hash<const type_plan*>{}(pair.first);
        const auto b = std::hash<const type_plan*>{}(pair.second);
        return a ^ (b + std::size_t{0x9e3779b9} + (a << 6U) + (a >> 2U));
    }
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

value_type type_access::bind_runtime_ownership(const value_type& type, opaque_ownership canonical) {
    assert(canonical.clone && canonical.destroy);
    /// Retains the next child to visit in one borrowed source plan.
    struct visit {
        const value_type* type; ///< Borrows a descriptor owned by the input graph.
        std::size_t next = 0;   ///< Counts already scheduled child edges.
    };
    std::vector<visit> pending{{&type}};
    std::unordered_map<const type_plan*, value_type> bound;
    // Memoized iterative postorder processes each node only after its children.
    while (!pending.empty()) {
        auto& current = pending.back();
        const auto* source = current.type->_plan.get();
        const auto children =
            source->fields.size() + source->element.has_value() + source->result.has_value();
        if (current.next != children) {
            const auto index = current.next++;
            const value_type* child = nullptr;
            if (index < source->fields.size()) {
                child = &source->fields[index];
            } else if (index == source->fields.size() && source->element) {
                child = &*source->element;
            } else {
                assert(source->result);
                child = &*source->result;
            }
            if (!bound.contains(child->_plan.get())) pending.push_back({child});
            continue;
        }
        bool changed = false;
        if (source->kind == type_kind::callable) {
            const auto operations = source->operations;
            if (operations.clone || operations.destroy) {
                if (operations.clone != canonical.clone || operations.destroy != canonical.destroy)
                    throw std::invalid_argument("foreign callable ownership operations");
            } else {
                changed = true;
            }
        }
        const auto transformed = [&](const value_type& child) -> const value_type& {
            return bound.at(child._plan.get());
        };
        const auto child_changed = [&](const value_type& child) {
            return !same_plan(child, transformed(child));
        };
        changed =
            changed || std::any_of(source->fields.begin(), source->fields.end(), child_changed);
        if (source->element) changed = changed || child_changed(*source->element);
        if (source->result) changed = changed || child_changed(*source->result);
        if (!changed) {
            bound.emplace(source, *current.type);
        } else {
            auto plan = std::make_shared<type_plan>(*source);
            std::transform(source->fields.begin(), source->fields.end(), plan->fields.begin(),
                           transformed);
            if (source->element) plan->element = transformed(*source->element);
            if (source->result) plan->result = transformed(*source->result);
            if (plan->kind == type_kind::callable) {
                plan->operations = canonical;
                plan->owns = true;
            } else if (plan->kind == type_kind::array) {
                assert(plan->element);
                plan->owns = plan->element->owns_values();
            } else if (plan->kind == type_kind::tuple || plan->kind == type_kind::record) {
                plan->owns =
                    std::all_of(plan->fields.begin(), plan->fields.end(),
                                [](const value_type& field) { return field.owns_values(); });
            }
            bound.emplace(source, value_type(std::move(plan)));
        }
        pending.pop_back();
    }
    return bound.at(type._plan.get());
}

opaque_ownership type_access::operations(const value_type& type) noexcept {
    assert(type.kind() == type_kind::callable);
    return type._plan->operations;
}

bool type_access::compatible_operations(const value_type& expected, const value_type& actual) {
    return compare_plans(expected, actual, false);
}

bool type_access::compare_plans(const value_type& expected,
                                const value_type& actual,
                                bool semantic) {
    using pair = std::pair<const type_plan*, const type_plan*>;
    if (expected._plan == actual._plan) return true;
    std::vector<pair> pending{{expected._plan.get(), actual._plan.get()}};
    std::unordered_set<pair, plan_pair_hash> visited;
    // Visit each distinct immutable plan pair once, including shared DAG edges.
    while (!pending.empty()) {
        const auto [a, b] = pending.back();
        pending.pop_back();
        if (a == b || !visited.insert({a, b}).second) continue;
        if (a->kind != b->kind) return false;
        if (semantic) {
            if (a->bytes.size != b->bytes.size || a->bytes.alignment != b->bytes.alignment ||
                a->leaf != b->leaf || a->identity != b->identity || a->names != b->names ||
                a->form != b->form || a->offsets != b->offsets ||
                a->element.has_value() != b->element.has_value() ||
                a->result.has_value() != b->result.has_value() ||
                a->fields.size() != b->fields.size())
                return false;
        } else if (a->kind == type_kind::callable) {
            if (a->operations.clone != b->operations.clone ||
                a->operations.destroy != b->operations.destroy)
                return false;
            continue;
        } else if (a->kind == type_kind::leaf || a->kind == type_kind::range) {
            continue;
        }
        if (a->element && b->element)
            pending.emplace_back(a->element->_plan.get(), b->element->_plan.get());
        if (semantic && a->result && b->result)
            pending.emplace_back(a->result->_plan.get(), b->result->_plan.get());
        // Retain only borrowed graph edges; root descriptors own all reachable plans.
        for (std::size_t i = 0; i != a->fields.size(); ++i)
            pending.emplace_back(a->fields[i]._plan.get(), b->fields[i]._plan.get());
    }
    return true;
}

std::size_t type_access::resources(const value_type& type) noexcept {
    return type._plan->resources;
}
bool type_access::parameter_allowed(const value_type& type) noexcept {
    return type._plan->parameter;
}

ownership_slot type_access::resource(const value_type& type, std::size_t ordinal) noexcept {
    assert(ordinal < resources(type));
    const value_type* current = &type;
    std::size_t offset = 0;
    // Cumulative carrier counts select one source-order path without a traversal stack.
    while (current->kind() == type_kind::tuple || current->kind() == type_kind::record) {
        const auto& plan = *current->_plan;
        const auto found =
            std::upper_bound(plan.resource_ends.begin(), plan.resource_ends.end(), ordinal);
        const auto i = static_cast<std::size_t>(found - plan.resource_ends.begin());
        assert(i < plan.fields.size());
        if (i == plan.fields.size()) std::terminate();
        if (i != 0) ordinal -= plan.resource_ends[i - 1];
        offset += plan.offsets[i];
        current = &plan.fields[i];
    }
    assert(current->kind() == type_kind::array || current->kind() == type_kind::callable);
    return {current, offset};
}

void require_compatible(const value_type& expected, const value_type& actual) {
    if (!expected.compatible_with(actual))
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
        plan.parameter = plan.parameter && type_access::parameter_allowed(field);
        const auto count = type_access::resources(field);
        if (count > std::numeric_limits<std::size_t>::max() - plan.resources)
            throw std::overflow_error("aggregate ownership count overflow");
        plan.resources += count;
        plan.resource_ends.push_back(plan.resources);
    }
    plan.bytes.size = align_offset(plan.bytes.size, plan.bytes.alignment);
}

/// Reads the cached built-in/composite parameter predicate.
bool parameter_allowed(const value_type& type) { return type_access::parameter_allowed(type); }

} // namespace cel::detail

namespace cel {

value_type::value_type(std::shared_ptr<const detail::type_plan> plan) : _plan(std::move(plan)) {}
value_type::value_type() : value_type(tuple({})) {}

void value_type::release_plan(std::shared_ptr<const detail::type_plan>& root) noexcept {
    auto pending = std::move(root);
    // Move dying descendants into their own worklist links before deleting any parent.
    while (pending) {
        auto current = std::move(pending);
        if (current.use_count() != 1) {
            current.reset();
            continue;
        }
        auto& plan = *const_cast<detail::type_plan*>(current.get());
        pending = std::move(plan.release_next);
        const auto detach = [&](value_type& child) {
            auto next = std::move(child._plan);
            if (next && next.use_count() == 1) {
                auto& node = *const_cast<detail::type_plan*>(next.get());
                node.release_next = std::move(pending);
                pending = std::move(next);
            }
        };
        if (plan.element) detach(*plan.element);
        if (plan.result) detach(*plan.result);
        for (auto& field : plan.fields)
            detach(field);
        current.reset();
    }
}
value_type::~value_type() { release_plan(_plan); }
value_type& value_type::operator=(const value_type& other) noexcept {
    if (this != &other) {
        auto next = other._plan;
        release_plan(_plan);
        _plan = std::move(next);
    }
    return *this;
}

value_type value_type::make_leaf(std::type_index identity, layout bytes) {
    (void)align_offset(bytes.size, bytes.alignment);
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::leaf;
    plan->bytes = bytes;
    plan->leaf = identity;
    plan->parameter = identity == typeid(bool) || identity == typeid(double);
    return value_type(std::move(plan));
}

value_type value_type::array(value_type element) {
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::array;
    plan->bytes = {sizeof(void*), alignof(void*)};
    plan->owns = element.owns_values();
    plan->parameter = element._plan->parameter;
    plan->resources = 1;
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
    plan->parameter = endpoint._plan->parameter;
    return value_type(std::move(plan));
}

value_type value_type::callable(std::span<const value_type> parameters, value_type result) {
    if (!std::all_of(parameters.begin(), parameters.end(), detail::parameter_allowed))
        throw std::invalid_argument("callable parameters require built-in composite values");
    auto plan = std::make_shared<detail::type_plan>();
    plan->kind = type_kind::callable;
    plan->bytes = {sizeof(void*), alignof(void*)};
    plan->owns = false;
    plan->parameter = false;
    plan->resources = 1;
    plan->fields.assign(parameters.begin(), parameters.end());
    plan->result = std::move(result);
    return value_type(std::move(plan));
}

bool value_type::same_shape(const value_type& other) const {
    return detail::type_access::compare_plans(*this, other, true);
}

bool value_type::compatible_with(const value_type& other) const {
    return same_shape(other) && detail::type_access::compatible_operations(*this, other);
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
