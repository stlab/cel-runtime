// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/value.hpp>

#include "storage.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace cel::detail {

/// Carries exactly one heap-array owner in trivially copyable canonical bytes.
/// Bytewise relocation transfers ownership and retires its source, never clones it.
struct array_carrier {
    std::vector<value>* owner; ///< Owns separately stored, non-relocated public wrappers.
};
static_assert(std::is_trivially_copyable_v<array_carrier>);
static_assert(sizeof(array_carrier) == sizeof(void*));
static_assert(alignof(array_carrier) == alignof(void*));

/// Reads a carrier without casting arbitrary raw bytes to a public wrapper.
array_carrier read_array(const std::byte* data) noexcept {
    array_carrier result{};
    std::memcpy(&result, data, sizeof(result));
    return result;
}

/// Returns an offset byte pointer, preserving null for zero-sized source unit.
/// Precondition: offset lies within the canonical extent, or is zero for null data.
template <class Byte>
Byte* byte_at(Byte* data, std::size_t offset) noexcept {
    assert(data || offset == 0);
    return data ? data + offset : nullptr;
}

/// Borrows the complete source-order child shape of a fixed aggregate.
/// Precondition: type is a tuple, record, or range and index is in bounds.
const value_type& child_type(const value_type& type, std::size_t index) {
    return type.kind() == type_kind::range ? type.element() : type.fields()[index];
}

/// Destroys an initialized aggregate prefix without allocating cleanup state.
/// Precondition: count is at most arity; only the first count children are initialized.
/// Complexity: O(initialized payload size) time and O(shape depth) call space.
void destroy_prefix(const value_type& type, std::byte* data, std::size_t count) noexcept {
    // Destroy only successfully constructed fields, in reverse construction order.
    while (count != 0) {
        --count;
        destroy_payload(child_type(type, count), byte_at(data, type.offsets()[count]));
    }
}

void clone_payload(const value_type& type, const std::byte* source, std::byte* destination) {
    switch (type.kind()) {
        case type_kind::leaf:
            std::memcpy(destination, source, type.byte_layout().size);
            return;
        case type_kind::array: {
            auto copy = std::make_unique<std::vector<value>>(*read_array(source).owner);
            const array_carrier carrier{copy.release()};
            std::memcpy(destination, &carrier, sizeof(carrier));
            return;
        }
        case type_kind::callable: {
            const auto operations = type_access::operations(type);
            if (!operations.clone)
                throw std::invalid_argument("callable ownership is not installed");
            operations.clone(source, destination);
            return;
        }
        case type_kind::tuple:
        case type_kind::record:
        case type_kind::range: {
            std::size_t completed = 0;
            try {
                // Construct each child exactly once; cleanup tracks the initialized prefix.
                for (const auto offset : type.offsets()) {
                    clone_payload(child_type(type, completed), byte_at(source, offset),
                                  byte_at(destination, offset));
                    ++completed;
                }
            } catch (...) {
                destroy_prefix(type, destination, completed);
                throw;
            }
        }
    }
}

void destroy_payload(const value_type& type, std::byte* data) noexcept {
    switch (type.kind()) {
        case type_kind::leaf:
            return;
        case type_kind::array:
            delete read_array(data).owner;
            return;
        case type_kind::callable: {
            const auto operations = type_access::operations(type);
            assert(operations.destroy);
            if (!operations.destroy) std::terminate();
            operations.destroy(data);
            return;
        }
        case type_kind::tuple:
        case type_kind::record:
        case type_kind::range:
            destroy_prefix(type, data, type.offsets().size());
            return;
    }
}

/// Owns outer aligned storage and the initialized child prefix.
/// Invariant: initialized counts fixed aggregate fields, or one leaf/array carrier.
/// Failed construction leaves an independently destructible prefix.
struct owned_payload {
    value_type type;             ///< Shares immutable canonical shape metadata.
    storage bytes;               ///< Owns aligned bytes, never public wrappers.
    std::size_t initialized = 0; ///< Counts successfully constructed top-level children.

    /// Allocates uninitialized canonical storage for the specified descriptor.
    /// Complexity: O(byte extent) zero-initialization time and storage.
    explicit owned_payload(value_type shape) : type(std::move(shape)) {
        const auto extent = type.byte_layout();
        bytes.reserve(extent.size, extent.alignment);
        bytes.resize(extent.size);
    }
    /// Destroys the initialized prefix before outer storage dies; never allocates.
    /// Complexity: O(initialized payload size) time and O(shape depth) call space.
    ~owned_payload() {
        if (type.kind() == type_kind::tuple || type.kind() == type_kind::record ||
            type.kind() == type_kind::range)
            destroy_prefix(type, bytes.data(), initialized);
        else if (initialized != 0)
            destroy_payload(type, bytes.data());
    }
};

const std::byte* value_access::data(const value& owner) noexcept {
    return owner._payload ? owner._payload->bytes.data() : nullptr;
}
value value_access::copy(const value_type& type, const std::byte* data) {
    auto result = std::make_unique<owned_payload>(type);
    clone_payload(type, data, result->bytes.data());
    result->initialized = type.kind() == type_kind::tuple || type.kind() == type_kind::record ||
                                  type.kind() == type_kind::range ?
                              type.offsets().size() :
                              1;
    return value(std::move(result));
}

/// Finds a source-order named field or reports malformed record access.
/// Throws std::invalid_argument for a non-record or missing name.
/// Complexity: O(field count + compared name bytes) time.
std::size_t field_index(const value_type& type, std::string_view name) {
    const auto names = type.field_names();
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) throw std::invalid_argument("record field does not exist");
    return static_cast<std::size_t>(found - names.begin());
}

} // namespace cel::detail

namespace cel {

value::value() noexcept = default;
value::~value() = default;
value::value(std::unique_ptr<detail::owned_payload> payload) : _payload(std::move(payload)) {}
value::value(value&& other) noexcept = default;
value::value(const value& other) {
    if (other._payload)
        *this = detail::value_access::copy(other.type(), detail::value_access::data(other));
}
value& value::operator=(const value& other) {
    if (this != &other) {
        value copy(other);
        _payload.swap(copy._payload);
    }
    return *this;
}
value& value::operator=(value&& other) noexcept {
    if (this != &other) _payload = std::move(other._payload);
    return *this;
}
const value_type& value::type() const {
    if (_payload) return _payload->type;
    static const value_type unit;
    return unit;
}
value value::from_leaf(value_type type, const void* source) {
    auto result = std::make_unique<detail::owned_payload>(std::move(type));
    std::memcpy(result->bytes.data(), source, result->type.byte_layout().size);
    result->initialized = 1;
    return value(std::move(result));
}
void value::copy_leaf(const value_type& expected, void* destination) const {
    if (!type().same_shape(expected)) throw std::invalid_argument("wrong concrete leaf type");
    std::memcpy(destination, detail::value_access::data(*this), expected.byte_layout().size);
}
const std::byte* value::raw_data(const value& owner) noexcept {
    return detail::value_access::data(owner);
}
std::span<const value> value::array_children(const std::byte* data) noexcept {
    return *detail::read_array(data).owner;
}
value value::array_owned(value_type element, std::vector<value> children) {
    // Validate carrier operations as well as semantic shape before taking ownership.
    for (const auto& child : children)
        detail::require_compatible(element, child.type());
    auto result = std::make_unique<detail::owned_payload>(value_type::array(element));
    auto owner = std::make_unique<std::vector<value>>(std::move(children));
    const detail::array_carrier carrier{owner.release()};
    std::memcpy(result->bytes.data(), &carrier, sizeof(carrier));
    result->initialized = 1;
    return value(std::move(result));
}
value value::tuple_owned(std::span<value> fields) {
    std::vector<value_type> types;
    types.reserve(fields.size());
    std::transform(fields.begin(), fields.end(), std::back_inserter(types),
                   [](const auto& child) { return child.type(); });
    auto result = std::make_unique<detail::owned_payload>(value_type::tuple(types));
    // Transfer canonical bytes exactly once and retire each boundary owner.
    for (auto& child : fields) {
        const auto index = result->initialized;
        const auto bytes = child.type().byte_layout().size;
        if (bytes != 0)
            std::memcpy(detail::byte_at(result->bytes.data(), result->type.offsets()[index]),
                        raw_data(child), bytes);
        if (child._payload) child._payload->initialized = 0;
        ++result->initialized;
    }
    return value(std::move(result));
}
value value::array(value_type element, std::span<const value> elements) {
    auto result = std::make_unique<detail::owned_payload>(value_type::array(element));
    auto children = std::make_unique<std::vector<value>>();
    children->reserve(elements.size());
    // Validate and copy in source order; vector ownership cleans up a failed prefix.
    for (const auto& child : elements) {
        detail::require_compatible(element, child.type());
        children->push_back(child);
    }
    const detail::array_carrier carrier{children.release()};
    std::memcpy(result->bytes.data(), &carrier, sizeof(carrier));
    result->initialized = 1;
    return value(std::move(result));
}
value value::aggregate(value_type type, std::span<const value> fields) {
    if (type.offsets().size() != fields.size())
        throw std::invalid_argument("wrong aggregate arity");
    auto result = std::make_unique<detail::owned_payload>(std::move(type));
    // Construct in source order; owned_payload destroys the initialized prefix on error.
    for (const auto& child : fields) {
        const auto index = result->initialized;
        const auto& expected = detail::child_type(result->type, index);
        detail::require_compatible(expected, child.type());
        detail::clone_payload(expected, detail::value_access::data(child),
                              detail::byte_at(result->bytes.data(), result->type.offsets()[index]));
        ++result->initialized;
    }
    return value(std::move(result));
}
value value::tuple(std::span<const value> fields) {
    std::vector<value_type> types;
    types.reserve(fields.size());
    std::transform(fields.begin(), fields.end(), std::back_inserter(types),
                   [](const auto& child) { return child.type(); });
    return aggregate(value_type::tuple(types), fields);
}
value value::record(value_type type, std::span<const value> fields) {
    if (type.kind() != type_kind::record) throw std::invalid_argument("not a record type");
    return aggregate(std::move(type), fields);
}
value value::range(value_type type, std::span<const value> endpoints) {
    if (type.kind() != type_kind::range) throw std::invalid_argument("not a range type");
    return aggregate(std::move(type), endpoints);
}
std::size_t value::size() const {
    if (type().kind() == type_kind::array)
        return detail::read_array(detail::value_access::data(*this)).owner->size();
    return type().offsets().size();
}
value value::at(std::size_t index) const {
    if (index >= size()) throw std::out_of_range("child index out of bounds");
    const auto* bytes = detail::value_access::data(*this);
    if (type().kind() == type_kind::array) return (*detail::read_array(bytes).owner)[index];
    return detail::value_access::copy(detail::child_type(type(), index),
                                      detail::byte_at(bytes, type().offsets()[index]));
}
void value::set(std::size_t index, const value& replacement) {
    if (index >= size()) throw std::out_of_range("child index out of bounds");
    const auto& expected =
        type().kind() == type_kind::array ? type().element() : detail::child_type(type(), index);
    detail::require_compatible(expected, replacement.type());
    if (type().kind() == type_kind::array) {
        (*detail::read_array(_payload->bytes.data()).owner)[index] = replacement;
        return;
    }
    value copy(replacement);
    auto* destination = detail::byte_at(_payload->bytes.data(), type().offsets()[index]);
    detail::destroy_payload(expected, destination);
    if (expected.byte_layout().size != 0)
        std::memcpy(destination, detail::value_access::data(copy), expected.byte_layout().size);
    // Retire the bytewise-transferred source without running its payload destructor.
    if (copy._payload) copy._payload->initialized = 0;
}
value value::field(std::string_view name) const { return at(detail::field_index(type(), name)); }
void value::set_field(std::string_view name, const value& replacement) {
    set(detail::field_index(type(), name), replacement);
}

} // namespace cel
