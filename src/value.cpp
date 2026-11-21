// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/value.hpp>

#include "callable_access.hpp"
#include "storage.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cel::detail {

struct array_owner;

namespace {
/// A byte-transferable canonical packed array owner.
///
/// Bytewise relocation transfers ownership and retires its source, never clones it.
struct array_carrier {
    array_owner* owner; ///< Owns one aligned allocation of canonical elements.
};
static_assert(std::is_trivially_copyable_v<array_carrier>);
static_assert(sizeof(array_carrier) == sizeof(void*));
static_assert(alignof(array_carrier) == alignof(void*));

/// Borrows an initialized canonical array's owner.
///
/// \pre data provides a readable initialized array-carrier extent with a live owner.
/// The returned carrier does not acquire ownership; its owner must remain alive while used.
array_carrier read_array(const std::byte* data) noexcept {
    array_carrier result{};
    std::memcpy(&result, data, sizeof(result));
    return result;
}

/// Writes one ownership carrier into canonical bytes.
///
/// \pre data is aligned uninitialized array-carrier storage; owner is a valid uniquely
/// owned array node. The caller retires its prior ownership of owner without destroying it.
void write_carrier(std::byte* data, array_owner* owner) noexcept {
    const array_carrier carrier{owner};
    std::memcpy(data, &carrier, sizeof(carrier));
}

/// Returns an offset byte pointer, preserving null for zero-sized source unit.
///
/// \pre offset lies within the canonical extent, or is zero for null data.
/// The returned pointer borrows the same allocation and has its lifetime.
template <class Byte>
Byte* byte_at(Byte* data, std::size_t offset) noexcept {
    assert(data || offset == 0);
    return data ? data + offset : nullptr;
}

} // namespace
/// Exclusive ownership of a partially initialized packed canonical array.
///
/// Destruction retires complete elements and any acquired resources in the next element.
struct array_owner {
    // [knowledge] Elements have canonical stride with no public wrapper or heap box.
    // initialized <= count; partial_resources belongs only to the next element.
    // pending_next and clone_source are temporary state for an active ownership operation.
    value_type element;                // Retains immutable element shape and destruction metadata.
    storage bytes;                     // Owns one suitably aligned canonical element allocation.
    std::size_t count;                 // Counts logical elements.
    std::size_t initialized = 0;       // Counts fully initialized elements.
    std::size_t partial_resources = 0; // Counts initialized carriers in the next element.
    array_owner* pending_next = nullptr;       // Links nodes during cloning or final destruction.
    const array_owner* clone_source = nullptr; // Borrows the source only during cloning.

    /// Allocates a checked canonical extent without constructing elements.
    ///
    /// Throws std::overflow_error if count times stride overflows.
    /// Complexity: O(1 + count times canonical element extent) time and result storage;
    /// zero-byte elements incur no per-element initialization.
    array_owner(value_type type, std::size_t length) : element(std::move(type)), count(length) {
        const auto layout = element.byte_layout();
        if (layout.size != 0 && count > std::numeric_limits<std::size_t>::max() / layout.size)
            throw std::overflow_error("array canonical extent overflow");
        const auto extent = count * layout.size;
        bytes.reserve(extent, layout.alignment);
        bytes.resize(extent);
    }
    /// Destroys initialized ownership without allocating or consuming nesting-dependent stack.
    ///
    /// Client operations retain their own costs and behavior.
    /// Complexity: linear in released value area; O(1) runtime auxiliary space.
    ~array_owner();
};

namespace {
/// Retires a traversal-order ownership-carrier prefix into enclosing retirement operations.
///
/// \pre Exactly the first count ownership carriers in data are valid and exclusively
/// owned; count <= type_access::resources(type). pending and queue remain alive until
/// their scheduled owners retire.
/// Source carrier ownership is retired; array storage is released when pending is drained.
/// Complexity: linear in carrier count, plus held destruction costs; O(1) runtime auxiliary space.
void retire_resources(const value_type& type,
                      std::byte* data,
                      std::size_t count,
                      array_owner*& pending,
                      retirement_queue& queue) noexcept {
    ownership_cursor cursor(type);
    // Scan the compiled sequence once, stopping at the initialized carrier prefix.
    while (count != 0) {
        const auto slot = cursor.next();
        if (!slot) continue;
        --count;
        auto* address = byte_at(data, slot->offset);
        if (slot->type->kind() == type_kind::array) {
            auto* node = read_array(address).owner;
            node->pending_next = pending;
            pending = node;
        } else if (callable_access::installed(*slot->type)) {
            callable_access::retire(address, queue);
        } else {
            const auto operations = type_access::operations(*slot->type);
            assert(operations.destroy);
            if (!operations.destroy) std::terminate();
            operations.destroy(address);
        }
    }
}

/// Retires a packed array's initialized prefix without deleting the array node itself.
///
/// \pre owner has exclusive valid prefix/partial ownership; pending and queue remain alive
/// until their scheduled owners retire.
/// Leaves no initialized element ownership in owner.
/// Complexity: linear in initialized carriers, plus held destruction costs; O(1) runtime auxiliary
/// space; scalar elements require no per-element retirement.
void retire_elements(array_owner& owner, array_owner*& pending, retirement_queue& queue) noexcept {
    const auto resources = type_access::resources(owner.element);
    const auto stride = owner.element.byte_layout().size;
    if (owner.partial_resources != 0)
        retire_resources(owner.element, byte_at(owner.bytes.data(), owner.initialized * stride),
                         owner.partial_resources, pending, queue);
    if (resources != 0) {
        // Visit only initialized elements; scalar arrays require no per-element cleanup.
        while (owner.initialized != 0)
            retire_resources(owner.element,
                             byte_at(owner.bytes.data(), --owner.initialized * stride), resources,
                             pending, queue);
    }
    owner.initialized = 0;
    owner.partial_resources = 0;
}

/// Releases all scheduled array ownership and leaves pending empty.
///
/// \pre pending links uniquely owned valid array nodes with discardable initialized
/// prefixes; queue remains alive until scheduled executable owners retire.
/// No runtime traversal allocation or nesting-dependent call space is used.
/// Complexity: linear in released value area; O(1) runtime auxiliary space.
void drain_arrays(array_owner*& pending, retirement_queue& queue) noexcept {
    // Detach each node before retiring its descendants and releasing its aligned bytes.
    while (pending) {
        auto* current = pending;
        pending = current->pending_next;
        current->pending_next = nullptr;
        retire_elements(*current, pending, queue);
        delete current;
    }
}

} // namespace
array_owner::~array_owner() {
    retirement_queue queue;
    array_owner* pending = nullptr;
    retire_elements(*this, pending, queue);
    drain_arrays(pending, queue);
}

namespace {
/// Validates cloning support for callable carriers outside arrays.
///
/// Throws std::invalid_argument if a direct callable lacks installed clone operations.
/// Complexity: linear in direct ownership carriers; O(1) auxiliary space.
void require_clone_operations(const value_type& type) {
    ownership_cursor cursor(type);
    // Check direct carriers without descending into possibly empty arrays.
    while (!cursor.empty()) {
        const auto slot = cursor.next();
        if (slot && slot->type->kind() == type_kind::callable &&
            !type_access::operations(*slot->type).clone)
            throw std::invalid_argument("callable ownership is not installed");
    }
}

/// Extends independently cloned carrier ownership through a fixed canonical extent.
///
/// \pre Direct callable carriers have clone operations; source meets clone_payload's
/// requirements. destination is disjoint aligned writable storage with exactly completed
/// valid cloned carriers.
/// completed <= type_access::resources(type); pending retains uniquely owned partial clones.
/// On success completed counts all direct carriers and pending retains descendants still
/// needing cloning. Failure preserves a discardable prefix counted by completed.
/// Complexity: linear in carriers and copied array bytes, plus held clone costs; new owned result
/// storage; O(1) runtime auxiliary space excluding opaque operations.
void clone_resources(const value_type& type,
                     const std::byte* source,
                     std::byte* destination,
                     std::size_t& completed,
                     array_owner*& pending) {
    const auto resources = type_access::resources(type);
    ownership_cursor cursor(type);
    std::size_t ordinal = 0;
    // Construct one independent carrier at a time without recursively cloning child arrays.
    while (completed != resources) {
        const auto slot = cursor.next();
        if (!slot || ordinal++ < completed) continue;
        const auto* input = byte_at(source, slot->offset);
        auto* output = byte_at(destination, slot->offset);
        if (slot->type->kind() == type_kind::array) {
            const auto* original = read_array(input).owner;
            assert(original->initialized == original->count && original->partial_resources == 0);
            const auto& element = slot->type->element();
            if (original->count != 0) require_clone_operations(element);
            auto copy = std::make_unique<array_owner>(element, original->count);
            if (copy->bytes.size() != 0)
                std::memcpy(copy->bytes.data(), original->bytes.data(), copy->bytes.size());
            copy->clone_source = original;
            copy->pending_next = pending;
            auto* owner = copy.release();
            write_carrier(output, owner);
            pending = owner;
        } else {
            const auto operations = type_access::operations(*slot->type);
            if (!operations.clone)
                throw std::invalid_argument("callable ownership is not installed");
            operations.clone(input, output);
        }
        ++completed;
    }
}

} // namespace
void clone_payload(const value_type& type, const std::byte* source, std::byte* destination) {
    require_clone_operations(type);
    const auto extent = type.byte_layout().size;
    if (extent != 0) std::memcpy(destination, source, extent);
    array_owner* pending = nullptr;
    std::size_t completed = 0;
    try {
        clone_resources(type, source, destination, completed, pending);
        // Newly owned array nodes retain their clone context and worklist state themselves.
        while (pending) {
            auto* current = pending;
            pending = current->pending_next;
            current->pending_next = nullptr;
            const auto* original = current->clone_source;
            const auto stride = current->element.byte_layout().size;
            if (type_access::resources(current->element) == 0) {
                current->initialized = current->count;
            } else {
                while (current->initialized != current->count) {
                    const auto offset = current->initialized * stride;
                    clone_resources(current->element, byte_at(original->bytes.data(), offset),
                                    byte_at(current->bytes.data(), offset),
                                    current->partial_resources, pending);
                    ++current->initialized;
                    current->partial_resources = 0;
                }
            }
            current->clone_source = nullptr;
        }
    } catch (...) {
        retirement_queue queue;
        array_owner* retired = nullptr;
        retire_resources(type, destination, completed, retired, queue);
        drain_arrays(retired, queue);
        throw;
    }
}

void destroy_payload(const value_type& type, std::byte* data) noexcept {
    retirement_queue queue;
    destroy_payload(type, data, queue);
}
void destroy_payload(const value_type& type, std::byte* data, retirement_queue& queue) noexcept {
    array_owner* pending = nullptr;
    retire_resources(type, data, type_access::resources(type), pending, queue);
    drain_arrays(pending, queue);
}

namespace {
/// Borrows a source-order fixed-aggregate child.
///
/// \pre type is a tuple, record, or range and index is in bounds.
/// The result's lifetime follows type's metadata views.
const value_type& child_type(const value_type& type, std::size_t index) {
    assert(index < type.offsets().size());
    return type.kind() == type_kind::range ? type.element() : type.fields()[index];
}

/// Destroys a successfully initialized field prefix without allocating cleanup state.
///
/// \pre type is a tuple, record, or range, count <= arity, and exactly that prefix is
/// valid owned data, exclusively accessible for retirement.
/// Complexity: linear in field count and released value area; O(1) runtime auxiliary space.
void destroy_prefix(const value_type& type, std::byte* data, std::size_t count) noexcept {
    while (count != 0) {
        --count;
        destroy_payload(child_type(type, count), byte_at(data, type.offsets()[count]));
    }
}

} // namespace
/// Exclusive ownership of a partially initialized canonical value.
struct owned_payload {
    // [knowledge] initialized counts complete aggregate fields, or one complete
    // scalar/array/callable representation; bytes stores canonical carriers, never wrappers.
    value_type type;             // Retains complete immutable destruction metadata.
    storage bytes;               // Owns canonical outer bytes, not public wrapper objects.
    std::size_t initialized = 0; // Counts fields, or one scalar/array/callable representation.

    /// Allocates checked canonical outer bytes without initializing ownership.
    ///
    /// Complexity: O(canonical byte extent) time and result storage.
    explicit owned_payload(value_type shape) : type(std::move(shape)) {
        const auto extent = type.byte_layout();
        bytes.reserve(extent.size, extent.alignment);
        bytes.resize(extent.size);
    }
    /// Destroys initialized ownership and releases outer storage without recursion/allocation.
    ///
    /// Client operations retain their own costs and behavior.
    /// Complexity: linear in field count and released value area; O(1) runtime auxiliary space.
    ~owned_payload() {
        if (type.kind() == type_kind::tuple || type.kind() == type_kind::record ||
            type.kind() == type_kind::range)
            destroy_prefix(type, bytes.data(), initialized);
        else if (initialized != 0)
            destroy_payload(type, bytes.data());
    }
};

namespace {
/// Returns the field count for fixed aggregates, or one for other canonical owners.
std::size_t outer_count(const value_type& type) {
    return type.kind() == type_kind::tuple || type.kind() == type_kind::record ||
                   type.kind() == type_kind::range ?
               type.offsets().size() :
               1;
}

} // namespace
const std::byte* value_access::data(const value& owner) noexcept {
    return owner._payload ? owner._payload->bytes.data() : nullptr;
}
void value_access::retire(value& owner, retirement_queue& queue) noexcept {
    if (!owner._payload) return;
    destroy_payload(owner._payload->type, owner._payload->bytes.data(), queue);
    owner._payload->initialized = 0;
    owner._payload.reset();
}
void value_access::release_to(value& owner, std::byte* destination) noexcept {
    if (!owner._payload) return;
    const auto size = owner._payload->type.byte_layout().size;
    if (size != 0) std::memcpy(destination, owner._payload->bytes.data(), size);
    owner._payload->initialized = 0;
    owner._payload.reset();
}
value value_access::take(const value_type& type, const std::byte* data) {
    auto result = std::make_unique<owned_payload>(type);
    const auto size = type.byte_layout().size;
    if (size != 0) std::memcpy(result->bytes.data(), data, size);
    result->initialized = outer_count(type);
    return value(std::move(result));
}
value value_access::copy(const value_type& type, const std::byte* data) {
    auto result = std::make_unique<owned_payload>(type);
    clone_payload(type, data, result->bytes.data());
    result->initialized = outer_count(type);
    return value(std::move(result));
}
value value_access::take_array(value_type element, std::span<value> children) {
    return value::array_owned(std::move(element), children);
}
array_builder::array_builder(const value_type& element, std::size_t count) :
    _owner(std::make_unique<array_owner>(element, count)) {}
array_builder::~array_builder() {
    if (!_owner) return;
    const auto stride = _owner->element.byte_layout().size;
    for (std::size_t i = _owner->count - _suffix; i != _owner->count; ++i)
        destroy_payload(_owner->element, byte_at(_owner->bytes.data(), i * stride));
}
std::byte* array_builder::destination() noexcept {
    assert(_owner && _suffix < _owner->count);
    return byte_at(_owner->bytes.data(),
                   (_owner->count - _suffix - 1) * _owner->element.byte_layout().size);
}
void array_builder::initialized() noexcept {
    assert(_owner && _suffix < _owner->count);
    ++_suffix;
}
void array_builder::release_to(std::byte* destination) noexcept {
    assert(_owner && _suffix == _owner->count);
    _owner->initialized = _owner->count;
    write_carrier(destination, _owner.release());
    _suffix = 0;
}
std::size_t value_access::array_size(const std::byte* data) noexcept {
    return read_array(data).owner->count;
}
void value_access::project(const value_type& type,
                           std::byte* source,
                           std::size_t index,
                           std::byte* destination) noexcept {
    if (type.kind() == type_kind::array) {
        auto* owner = read_array(source).owner;
        const auto stride = owner->element.byte_layout().size;
        if (stride != 0)
            std::memcpy(destination, byte_at(owner->bytes.data(), index * stride), stride);
        // The selected element transfers; only siblings retain ownership to retire.
        for (std::size_t i = 0; i != owner->count; ++i)
            if (i != index)
                destroy_payload(owner->element, byte_at(owner->bytes.data(), i * stride));
        owner->initialized = 0;
        delete owner;
    } else {
        const auto fields = type.fields();
        const auto size = fields[index].byte_layout().size;
        if (size != 0) std::memcpy(destination, byte_at(source, type.offsets()[index]), size);
        for (std::size_t i = 0; i != fields.size(); ++i)
            if (i != index) destroy_payload(fields[i], byte_at(source, type.offsets()[i]));
    }
}

namespace {
/// Returns the source-order index of a named record field.
///
/// Throws std::invalid_argument for a non-record or missing name.
/// Complexity: O(field count + compared name bytes) time.
std::size_t field_index(const value_type& type, std::string_view name) {
    const auto names = type.field_names();
    const auto found = std::find(names.begin(), names.end(), name);
    if (found == names.end()) throw std::invalid_argument("record field does not exist");
    return static_cast<std::size_t>(found - names.begin());
}

} // namespace
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
value value::from_concrete(value_type type,
                           const void* source,
                           void (*write)(const void*, const value_type&, std::byte*)) {
    auto result = std::make_unique<detail::owned_payload>(std::move(type));
    write(source, result->type, result->bytes.data());
    result->initialized = detail::outer_count(result->type);
    return value(std::move(result));
}
void value::copy_leaf(const value_type& expected, void* destination) const {
    if (!type().same_shape(expected)) throw std::invalid_argument("wrong concrete leaf type");
    std::memcpy(destination, detail::value_access::data(*this), expected.byte_layout().size);
}
const std::byte* value::raw_data(const value& owner) noexcept {
    return detail::value_access::data(owner);
}
value value::copy_canonical(const value_type& type, const std::byte* data) {
    return detail::value_access::copy(type, data);
}
void value::release_canonical(value& source, std::byte* destination) noexcept {
    detail::value_access::release_to(source, destination);
}
detail::array_view value::array_data(const std::byte* data) noexcept {
    const auto* owner = detail::read_array(data).owner;
    return {owner->element, owner->bytes.data(), owner->count, owner->element.byte_layout().size};
}
void value::write_array(const value_type& element,
                        std::byte* destination,
                        std::size_t count,
                        const void* source,
                        void (*write)(const void*, std::size_t, const value_type&, std::byte*)) {
    auto owner = std::make_unique<detail::array_owner>(element, count);
    const auto stride = element.byte_layout().size;
    // The owner tracks completed elements; a throwing writer cleans its own partial element.
    while (owner->initialized != count) {
        write(source, owner->initialized, element,
              detail::byte_at(owner->bytes.data(), owner->initialized * stride));
        ++owner->initialized;
    }
    detail::write_carrier(destination, owner.release());
}
void value::destroy_fields(const value_type& type, std::byte* data, std::size_t count) noexcept {
    detail::destroy_prefix(type, data, count);
}
value value::array_owned(value_type element, std::span<value> children) {
    auto result = std::make_unique<detail::owned_payload>(value_type::array(element));
    auto owner = std::make_unique<detail::array_owner>(element, children.size());
    const auto stride = element.byte_layout().size;
    detail::compatibility_context comparisons;
    // Transfer each canonical element and retire its public boundary owner.
    for (auto& child : children) {
        comparisons.require(element, child.type());
        if (stride != 0)
            std::memcpy(detail::byte_at(owner->bytes.data(), owner->initialized * stride),
                        raw_data(child), stride);
        ++owner->initialized;
        if (child._payload) child._payload->initialized = 0;
        child._payload.reset();
    }
    detail::write_carrier(result->bytes.data(), owner.release());
    result->initialized = 1;
    return value(std::move(result));
}
value value::array(value_type element, std::span<const value> elements) {
    auto result = std::make_unique<detail::owned_payload>(value_type::array(element));
    auto owner = std::make_unique<detail::array_owner>(element, elements.size());
    const auto stride = element.byte_layout().size;
    detail::compatibility_context comparisons;
    // Clone directly into packed canonical element bytes, tracking only the initialized prefix.
    for (const auto& child : elements) {
        comparisons.require(element, child.type());
        detail::clone_payload(element, raw_data(child),
                              detail::byte_at(owner->bytes.data(), owner->initialized * stride));
        ++owner->initialized;
    }
    detail::write_carrier(result->bytes.data(), owner.release());
    result->initialized = 1;
    return value(std::move(result));
}
value value::aggregate(value_type type, std::span<const value> fields) {
    if (type.offsets().size() != fields.size())
        throw std::invalid_argument("wrong aggregate arity");
    auto result = std::make_unique<detail::owned_payload>(std::move(type));
    detail::compatibility_context comparisons;
    // Completed children are independently owned; later failures destroy precisely that prefix.
    for (const auto& child : fields) {
        const auto index = result->initialized;
        const auto& expected = detail::child_type(result->type, index);
        comparisons.require(expected, child.type());
        detail::clone_payload(expected, raw_data(child),
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
    if (type().kind() == type_kind::array) return array_data(raw_data(*this)).size;
    return type().offsets().size();
}
value value::at(std::size_t index) const {
    if (index >= size()) throw std::out_of_range("child index out of bounds");
    const auto* bytes = raw_data(*this);
    if (type().kind() == type_kind::array) {
        const auto view = array_data(bytes);
        return detail::value_access::copy(view.element,
                                          detail::byte_at(view.data, index * view.stride));
    }
    return detail::value_access::copy(detail::child_type(type(), index),
                                      detail::byte_at(bytes, type().offsets()[index]));
}
void value::set(std::size_t index, const value& replacement) {
    if (index >= size()) throw std::out_of_range("child index out of bounds");
    const auto& expected =
        type().kind() == type_kind::array ? type().element() : detail::child_type(type(), index);
    detail::compatibility_context comparisons;
    comparisons.require(expected, replacement.type());
    value copy(replacement);
    std::byte* destination = nullptr;
    if (type().kind() == type_kind::array) {
        auto* owner = detail::read_array(_payload->bytes.data()).owner;
        destination = detail::byte_at(owner->bytes.data(), index * expected.byte_layout().size);
    } else {
        destination = detail::byte_at(_payload->bytes.data(), type().offsets()[index]);
    }
    detail::destroy_payload(expected, destination);
    if (expected.byte_layout().size != 0)
        std::memcpy(destination, raw_data(copy), expected.byte_layout().size);
    // Retire the transferred source without destroying its canonical payload.
    if (copy._payload) copy._payload->initialized = 0;
}
value value::field(std::string_view name) const { return at(detail::field_index(type(), name)); }
void value::set_field(std::string_view name, const value& replacement) {
    set(detail::field_index(type(), name), replacement);
}

} // namespace cel

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include "canonical.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#endif
#elif defined(__SANITIZE_ADDRESS__)
#include <sanitizer/asan_interface.h>
#endif

namespace {
/// Requests a test callback failure after acquiring a temporary resource.
bool fail_test_clone = false;
/// Counts live resources created by the synthetic opaque clone callback.
int live_test_owners = 0;
/// Requests a failure after a known initialized ownership prefix.
int test_clones_until_failure = -1;
/// Identifies a synthetic clone failure without allocating a diagnostic.
struct test_clone_error : std::exception {
    /// Returns a fixed diagnostic without allocating.
    const char* what() const noexcept override { return "synthetic clone failure"; }
};

/// Clones a synthetic opaque owner without implementing a callable object.
void clone_test_owner(const std::byte* source, std::byte* destination) {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), source, sizeof(pointer));
    auto copy = std::make_unique<int>(*pointer);
    if (fail_test_clone) throw std::runtime_error("synthetic clone failure");
    if (test_clones_until_failure == 0) {
        throw test_clone_error{};
    }
    if (test_clones_until_failure > 0) --test_clones_until_failure;
    ++live_test_owners;
    pointer = copy.release();
    std::memcpy(destination, static_cast<const void*>(&pointer), sizeof(pointer));
}

/// Destroys a synthetic opaque owner without allocating.
void destroy_test_owner(std::byte* data) noexcept {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), data, sizeof(pointer));
    delete pointer;
    --live_test_owners;
}

/// Supplies a distinct installed operation pair to test representation admission.
void destroy_test_owner_alternative(std::byte* data) noexcept { destroy_test_owner(data); }

/// Catches repeating complete comparisons for every empty element with shared metadata.
TEST_CASE("checked arrays reuse equivalent descriptor comparisons across elements") {
    constexpr std::size_t depth = 128;
    constexpr std::size_t count = 128;
    auto expected = cel::value_type::leaf<bool>();
    auto actual = cel::value_type::leaf<bool>();
    for (std::size_t i = 0; i != depth; ++i) {
        expected = cel::value_type::array(expected);
        actual = cel::value_type::array(actual);
    }
    const auto empty = cel::value::array(actual.element(), {});
    const std::vector<cel::value> children(count, empty);
    cel::value result;
    result = cel::value::array(expected, children);
    CHECK(result.size() == count);
    CHECK(result.at(count - 1).size() == 0);

    auto consumed = children;
    result = cel::detail::value_access::take_array(expected, consumed);
    for (const auto& child : consumed)
        CHECK(child.type().same_shape(cel::value{}.type()));

    cel::detail::compatibility_context comparisons;
    std::size_t work = 0;
    for (const auto& child : children)
        work += comparisons.require(expected, child.type());
    CHECK(work > 0);
    CHECK(work <= 2 * (depth + 1));

    for (std::size_t i = 0; i != count; ++i) {
        const auto distinct = cel::value::array(actual.element(), {});
        work += comparisons.require(expected, distinct.type());
    }
    CHECK(work <= 2 * (depth + count + 1));
}

/// Catches memo entries authorizing unequal shapes or foreign operations after a failed check.
TEST_CASE("operation comparison caches keep shape and ownership checks independent") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto expected = cel::detail::type_access::install_callable_ownership(
        signature, {clone_test_owner, destroy_test_owner});
    cel::detail::compatibility_context comparisons;
    for (int i = 0; i != 128; ++i) {
        const auto actual = cel::detail::type_access::install_callable_ownership(
            signature,
            {clone_test_owner, i % 2 == 0 ? destroy_test_owner : destroy_test_owner_alternative});
        if (i % 2 == 0) {
            CHECK_NOTHROW(comparisons.require(expected, actual));
        } else {
            CHECK_THROWS_AS(comparisons.require(expected, actual), std::invalid_argument);
            CHECK_THROWS_AS(comparisons.require(expected, actual), std::invalid_argument);
        }
    }
    const auto one = cel::value_type::tuple(std::array{expected});
    const auto two = cel::value_type::tuple(std::array{expected, expected});
    CHECK_THROWS_AS(comparisons.require(one, two), std::invalid_argument);
    CHECK_THROWS_AS(comparisons.require(one, two), std::invalid_argument);
    CHECK_NOTHROW(comparisons.require(one, cel::value_type::tuple(std::array{expected})));

    const auto foreign = cel::detail::type_access::install_callable_ownership(
        signature, {clone_test_owner, destroy_test_owner_alternative});
    const auto valid_empty = cel::value::array(expected, {});
    const auto foreign_empty = cel::value::array(foreign, {});
    CHECK_THROWS_AS(cel::value::array(valid_empty.type(), std::array{valid_empty, foreign_empty}),
                    std::invalid_argument);
    auto destination = cel::value::array(valid_empty.type(), std::array{valid_empty});
    CHECK_THROWS_AS(destination.set(0, foreign_empty), std::invalid_argument);
    CHECK(destination.at(0).type().compatible_with(valid_empty.type()));
}

/// Catches leaking a partially cloned traversal-order prefix.
TEST_CASE("partial ownership traversal cleans its initialized prefix") {
    const auto installed = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<double>()),
        {clone_test_owner, destroy_test_owner});
    int number = 42;
    int* pointer = &number;
    std::array<std::byte, sizeof(pointer)> bytes;
    std::memcpy(bytes.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    const auto callable = cel::detail::value_access::copy(installed, bytes.data());
    const auto array = cel::value::from(std::vector<double>{2.0, 3.0});
    const auto source =
        cel::value::tuple(std::array{callable, array, callable, array, callable, array});
    const auto live = live_test_owners;
    test_clones_until_failure = 2;
    bool caught = false;
    try {
        auto copy = source;
    } catch (const test_clone_error&) {
        caught = true;
    }
    test_clones_until_failure = -1;
    CHECK(caught);
    CHECK(live_test_owners == live);
    CHECK(source.at(1).at(0).as<double>() == 2.0);
}
/// Catches conflating semantic signatures with stored opaque representation compatibility.
TEST_CASE("public compatibility rejects nested foreign callable ownership operations") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto installed = cel::detail::type_access::install_callable_ownership(
        signature, {clone_test_owner, destroy_test_owner});
    const auto foreign = cel::detail::type_access::install_callable_ownership(
        signature, {clone_test_owner, destroy_test_owner_alternative});
    CHECK(signature.compatible_with(signature));
    CHECK_FALSE(signature.owns_values());
    CHECK(installed.compatible_with(installed));
    CHECK(installed.same_shape(foreign));
    CHECK_FALSE(installed.compatible_with(foreign));
    CHECK_FALSE(foreign.compatible_with(installed));
    const auto nested = cel::value_type::array(cel::value_type::record(
        "callbacks",
        std::array{cel::record_field{"stored", cel::value_type::tuple(std::array{installed})}}));
    const auto nested_foreign = cel::value_type::array(cel::value_type::record(
        "callbacks",
        std::array{cel::record_field{"stored", cel::value_type::tuple(std::array{foreign})}}));
    CHECK(nested.same_shape(nested_foreign));
    CHECK_FALSE(nested.compatible_with(nested_foreign));
    CHECK_FALSE(nested_foreign.compatible_with(nested));
    CHECK(live_test_owners == 0);
}

/// Catches accepting incomplete opaque operations or mutating shared capability metadata.
TEST_CASE("private callable ownership installation preserves shape and validates operation pairs") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<double>());
    CHECK_THROWS_AS(cel::detail::require_ownership(signature), std::invalid_argument);
    cel::detail::storage destination_bytes;
    destination_bytes.reserve(signature.byte_layout().size, signature.byte_layout().alignment);
    destination_bytes.resize(signature.byte_layout().size);
    CHECK_THROWS_AS(cel::detail::clone_payload(signature, nullptr, destination_bytes.data()),
                    std::invalid_argument);
    CHECK_THROWS_AS(cel::detail::type_access::install_callable_ownership(
                        signature, {clone_test_owner, nullptr}),
                    std::invalid_argument);
    CHECK_THROWS_AS(cel::detail::type_access::install_callable_ownership(
                        cel::value_type::leaf<double>(), {clone_test_owner, destroy_test_owner}),
                    std::invalid_argument);
    const auto installed = cel::detail::type_access::install_callable_ownership(
        signature, {clone_test_owner, destroy_test_owner});
    CHECK_FALSE(signature.owns_values());
    CHECK(installed.owns_values());
    CHECK(signature.same_shape(installed));
    CHECK_NOTHROW(cel::detail::require_ownership(installed));
    CHECK_THROWS_AS(cel::detail::require_ownership(cel::value_type::array(signature)),
                    std::invalid_argument);
    CHECK(live_test_owners == 0);
    {
        auto original = std::make_unique<int>(42);
        int* pointer = original.get();
        std::array<std::byte, sizeof(pointer)> carrier{};
        std::memcpy(carrier.data(), static_cast<const void*>(&pointer), sizeof(pointer));
        auto owner = cel::detail::value_access::copy(installed, carrier.data());
        *original = 99;
        auto copy = owner;
        int* owned = nullptr;
        int* copied = nullptr;
        std::memcpy(static_cast<void*>(&owned), cel::detail::value_access::data(owner),
                    sizeof(owned));
        std::memcpy(static_cast<void*>(&copied), cel::detail::value_access::data(copy),
                    sizeof(copied));
        CHECK(*owned == 42);
        CHECK(*copied == 42);
        *owned = 17;
        CHECK(*copied == 42);
        *copied = 18;
        CHECK(*owned == 17);
        const auto deferred_record =
            cel::value_type::record("code", std::array{cel::record_field{"body", signature}});
        CHECK_THROWS_AS(cel::value::record(deferred_record, std::array{owner}),
                        std::invalid_argument);
        const auto owned_record =
            cel::value_type::record("code", std::array{cel::record_field{"body", installed}});
        CHECK(
            cel::value::record(owned_record, std::array{owner}).type().same_shape(deferred_record));
        const auto alternative = cel::detail::type_access::install_callable_ownership(
            signature, {clone_test_owner, destroy_test_owner_alternative});
        CHECK(alternative.same_shape(installed));
        CHECK_THROWS_AS(
            cel::value::record(
                cel::value_type::record("code", std::array{cel::record_field{"body", alternative}}),
                std::array{owner}),
            std::invalid_argument);
        auto children = std::array{cel::value::from(std::vector<double>{1.0, 2.0}), owner};
        const auto failing_record = cel::value_type::record(
            "partial", std::array{cel::record_field{"items", children[0].type()},
                                  cel::record_field{"code", installed}});
        const auto good_record = cel::value::record(failing_record, children);
        const auto packed_records =
            cel::value::array(failing_record, std::array{good_record, good_record});
        auto replacement_record = good_record;
        auto replacement_array = cel::value::array(installed, std::array{owner});
        const auto live_before = live_test_owners;
        auto destination = cel::value::from(false);
        fail_test_clone = true;
        CHECK_THROWS_AS(cel::value::record(failing_record, children), std::runtime_error);
        CHECK_THROWS_AS(cel::value{good_record}, std::runtime_error);
        CHECK_THROWS_AS(cel::value{packed_records}, std::runtime_error);
        CHECK_THROWS_AS(destination = good_record, std::runtime_error);
        CHECK_THROWS_AS(replacement_record.set_field("code", owner), std::runtime_error);
        CHECK_THROWS_AS(replacement_array.set(0, owner), std::runtime_error);
        fail_test_clone = false;
        CHECK(live_test_owners == live_before);
        CHECK_FALSE(destination.as<bool>());
        int* preserved = nullptr;
        auto preserved_field = replacement_record.field("code");
        std::memcpy(static_cast<void*>(&preserved),
                    cel::detail::value_access::data(preserved_field), sizeof(preserved));
        CHECK(*preserved == 17);
        preserved_field = replacement_array.at(0);
        std::memcpy(static_cast<void*>(&preserved),
                    cel::detail::value_access::data(preserved_field), sizeof(preserved));
        CHECK(*preserved == 17);
    }
    CHECK(live_test_owners == 0);
}
/// Owns a raw clone solely to verify private raw ownership contracts.
struct raw_owner {
    cel::value_type type;       ///< Retains immutable destruction metadata.
    cel::detail::storage bytes; ///< Owns aligned outer storage.
    bool initialized = false;   ///< Tracks whether payload destruction is required.
    /// Clones a canonical payload with clone_payload's source requirements.
    ///
    /// Complexity: linear in source value area, plus held clone costs; new owned storage.
    raw_owner(const cel::value_type& shape, const std::byte* source) : type(shape) {
        const auto layout = type.byte_layout();
        bytes.reserve(layout.size, layout.alignment);
        bytes.resize(layout.size);
        cel::detail::clone_payload(type, source, bytes.data());
        initialized = true;
    }
    /// Copies a public value into raw canonical storage.
    /// Complexity: O(payload size) time and space.
    explicit raw_owner(const cel::value& source) :
        raw_owner(source.type(), cel::detail::value_access::data(source)) {}
    /// Destroys exactly one raw payload without allocating.
    /// Complexity: O(payload size) time and O(shape depth) call space.
    ~raw_owner() {
        if (initialized) cel::detail::destroy_payload(type, bytes.data());
    }
};

/// Catches reading a fixed aggregate before checking its deferred callable carriers.
TEST_CASE("deferred callable cloning rejects fixed aggregates before reading source") {
    const auto deferred = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto tuple = cel::value_type::tuple(std::array{cel::value_type::leaf<bool>(), deferred});
    const auto record =
        cel::value_type::record("callbacks", std::array{cel::record_field{"nested", tuple}});
    CHECK_THROWS_AS(raw_owner(tuple, nullptr), std::invalid_argument);
    CHECK_THROWS_AS(raw_owner(record, nullptr), std::invalid_argument);
}

/// Catches copying packed element bytes before rejecting their deferred callable descriptor.
TEST_CASE("deferred callable cloning rejects array elements before reading their bytes") {
    const auto deferred = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto installed = cel::detail::type_access::install_callable_ownership(
        deferred, {clone_test_owner, destroy_test_owner});
    int number = 42;
    int* pointer = &number;
    std::array<std::byte, sizeof(pointer)> carrier;
    std::memcpy(carrier.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    cel::detail::canonical_owner source(cel::value_type::array(installed));
    cel::detail::array_builder builder(installed, 1);
    auto* element = builder.destination();
    cel::detail::clone_payload(installed, carrier.data(), element);
    builder.initialized();
    builder.release_to(source.bytes.data());
    source.live = true;
    const auto live = live_test_owners;

#ifdef ASAN_POISON_MEMORY_REGION
    ASAN_POISON_MEMORY_REGION(element, installed.byte_layout().size);
    // Restore readability before source destruction, including on an unexpected exception.
    const auto unpoison = [size = installed.byte_layout().size](std::byte* data) noexcept {
        ASAN_UNPOISON_MEMORY_REGION(data, size);
    };
    const std::unique_ptr<std::byte, decltype(unpoison)> readable_on_exit(element, unpoison);
#endif

    CHECK_THROWS_AS(raw_owner(cel::value_type::array(deferred), source.bytes.data()),
                    std::invalid_argument);
    CHECK(live_test_owners == live);
}

/// Catches replacing a requested deferred descriptor with installed source-owner metadata.
TEST_CASE("deferred callable cloning rejects fixed aggregates inside nested arrays") {
    const auto deferred = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto installed = cel::detail::type_access::install_callable_ownership(
        deferred, {clone_test_owner, destroy_test_owner});
    int number = 42;
    int* pointer = &number;
    std::array<std::byte, sizeof(pointer)> carrier;
    std::memcpy(carrier.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    const auto callable = cel::detail::value_access::copy(installed, carrier.data());
    auto source = cel::value::tuple(std::array{cel::value::from(true), callable});
    auto requested = cel::value_type::tuple(std::array{cel::value_type::leaf<bool>(), deferred});
    for (int depth = 0; depth != 3; ++depth) {
        source = cel::value::array(source.type(), std::array{source});
        requested = cel::value_type::array(requested);
        const auto live = live_test_owners;
        CHECK_THROWS_AS(raw_owner(requested, cel::detail::value_access::data(source)),
                        std::invalid_argument);
        CHECK(live_test_owners == live);
    }
}

/// Catches retaining an earlier sibling clone after a nested deferred-callable rejection.
TEST_CASE("deferred callable cloning releases the initialized sibling prefix") {
    const auto deferred = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto installed = cel::detail::type_access::install_callable_ownership(
        deferred, {clone_test_owner, destroy_test_owner});
    int number = 42;
    int* pointer = &number;
    std::array<std::byte, sizeof(pointer)> carrier;
    std::memcpy(carrier.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    const auto callable = cel::detail::value_access::copy(installed, carrier.data());
    const auto array = cel::value::array(installed, std::array{callable});
    const auto source = cel::value::tuple(std::array{callable, array});
    const auto requested =
        cel::value_type::tuple(std::array{installed, cel::value_type::array(deferred)});
    const auto live = live_test_owners;
    CHECK_THROWS_AS(raw_owner(requested, cel::detail::value_access::data(source)),
                    std::invalid_argument);
    CHECK(live_test_owners == live);
    CHECK(source.at(1).size() == 1);
}

/// Catches rejecting uninhabited deferred element types, including beneath a nonempty array.
TEST_CASE("deferred callable cloning preserves empty and nested empty arrays") {
    const auto deferred = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto empty = cel::value::array(deferred, {});
    raw_owner empty_copy(empty);
    CHECK(cel::detail::value_access::array_size(empty_copy.bytes.data()) == 0);
    const auto nested = cel::value::array(empty.type(), std::array{empty});
    raw_owner nested_copy(nested);
    const auto result = cel::detail::value_access::copy(nested_copy.type, nested_copy.bytes.data());
    CHECK(result.size() == 1);
    CHECK(result.at(0).size() == 0);
    CHECK(result.type().element().element().kind() == cel::type_kind::callable);
}

/// Catches raw clones sharing mutable arrays and duplicate destruction after byte transfer.
TEST_CASE("private canonical clone destruction and transfer preserve exactly one owner") {
    auto source = cel::value::from(std::tuple{std::vector<double>{1.0, 2.0}, true});
    raw_owner raw(source);
    auto copied = cel::detail::value_access::copy(raw.type, raw.bytes.data());
    auto child = source.at(0);
    child.set(0, cel::value::from(8.0));
    source.set(0, child);
    CHECK(copied.at(0).at(0).as<double>() == 1.0);
    CHECK(cel::detail::value_access::copy(raw.type, raw.bytes.data()).at(0).at(0).as<double>() ==
          1.0);
    cel::detail::storage transferred;
    const auto extent = raw.type.byte_layout();
    transferred.reserve(extent.size, extent.alignment);
    transferred.resize(extent.size);
    std::memcpy(transferred.data(), raw.bytes.data(), extent.size);
    raw.initialized = false;
    CHECK(cel::detail::value_access::copy(raw.type, transferred.data()).at(1).as<bool>());
    cel::detail::destroy_payload(raw.type, transferred.data());
    raw_owner empty(cel::value{});
    CHECK(cel::detail::value_access::copy(empty.type, empty.bytes.data()).size() == 0);
}
} // namespace

#endif
