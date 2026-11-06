// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_VALUE_HPP
#define CEL_RUNTIME_VALUE_HPP

#include <cel/runtime/value_type.hpp>

#include <array>
#include <bit>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace cel {
/// Provides trusted concrete boundary adapters; these are not client-leaf registrations.
namespace detail {
struct owned_payload;
struct value_access;
template <class T>
struct concrete_adapter;
} // namespace detail

/// Owns a complete runtime value; copies own independent mutable payloads.
/// Moved-from and default values are source unit. Internal descriptors may be shared.
/// Opaque leaves own their bytes, not objects reachable through client pointers.
/// Public wrappers must never be byte-relocated; private carriers transfer one owner.
/// Composite admission rejects incompatible opaque ownership operations even for equal shapes.
/// Example: `auto number = value::from(2.0); auto copy = number;`.
class value {
    std::unique_ptr<detail::owned_payload> _payload;
    friend struct detail::value_access;
    template <class T>
    friend struct detail::concrete_adapter;

    /// Takes uniquely owned, fully initialized payload storage.
    explicit value(std::unique_ptr<detail::owned_payload> payload);
    /// Copies a trusted concrete leaf representation into aligned storage.
    /// Complexity: O(type byte size) time and storage.
    static value from_leaf(value_type type, const void* source);
    /// Copies a leaf into its trusted adapter; throws std::invalid_argument on type mismatch.
    /// Complexity: O(type byte size) time.
    void copy_leaf(const value_type& expected, void* destination) const;
    /// Builds a fixed aggregate, validating every child as it is copied.
    /// Throws std::invalid_argument for wrong arity or child shape.
    /// Failure destroys every initialized child without allocating rollback copies.
    /// Complexity: O(payload size) time and owned space.
    static value aggregate(value_type type, std::span<const value> fields);
    /// Takes already-owned boundary children without cloning their mutable payloads.
    /// Throws std::invalid_argument for wrong shapes; failure releases all children.
    /// Complexity: O(element count + compared shape size) time and O(element count) wrapper
    /// storage.
    static value array_owned(value_type element, std::vector<value> children);
    /// Takes already-owned tuple boundary fields into canonical storage.
    /// Complexity: O(field byte extents + field count) time and canonical outer storage.
    static value tuple_owned(std::span<value> fields);
    /// Borrows canonical bytes from a live public owner.
    static const std::byte* raw_data(const value& owner) noexcept;
    /// Borrows non-relocated array children from a live canonical carrier.
    /// Precondition: data contains an initialized array carrier.
    static std::span<const value> array_children(const std::byte* data) noexcept;

public:
    /// Creates source unit without allocating payload storage.
    value() noexcept;
    /// Destroys owned children and releases storage without allocating.
    /// Complexity: O(owned payload size) time and O(shape depth) call space.
    ~value();
    /// Copies all mutable payloads independently; shares only immutable metadata.
    /// Complexity: O(owned payload size) time and space.
    value(const value& other);
    /// Transfers ownership and leaves other as source unit.
    value(value&& other) noexcept;
    /// Replaces ownership with an independent copy, including self-assignment.
    /// Failure leaves this unchanged without copying its prior payload.
    /// Complexity: O(combined payload size) time and O(source payload size) new space.
    value& operator=(const value& other);
    /// Releases old ownership, transfers other, and leaves other as unit; self-move is unchanged.
    /// Complexity: O(old payload size) destruction time.
    value& operator=(value&& other) noexcept;
    /// Borrows the complete descriptor until this value is assigned or destroyed.
    /// Example: `value::from(true).type().kind() == type_kind::leaf`.
    const value_type& type() const;
    /// Copies a concrete leaf or owning vector/tuple through its trusted adapter.
    /// Nontrivial opaque leaves fail compilation; vector/tuple facades are supported.
    /// Complexity: O(payload size) time and owned space.
    /// Example: `auto numbers = value::from(std::vector<double>{1.0, 2.0});`.
    template <class T>
    static value from(const T& source) {
        return detail::concrete_adapter<std::remove_cv_t<T>>::from(source);
    }
    /// Returns an independent concrete copy through its trusted adapter.
    /// Throws std::invalid_argument if the complete concrete type does not match.
    /// Complexity: O(payload size) time and owned result space.
    /// Example: `value::from(2.0).as<double>() == 2.0`.
    template <class T>
    T as() const {
        return detail::concrete_adapter<std::remove_cv_t<T>>::as(*this);
    }
    /// Copies a sequence of exactly the specified element shape.
    /// Throws std::invalid_argument for a mismatched element, releasing earlier copies.
    /// Complexity: O(payload size + element count) time and owned space.
    /// Example: `auto empty = value::array(value_type::leaf<double>(), {});`.
    static value array(value_type element, std::span<const value> elements);
    /// Copies source-order fields into a canonical tuple; empty fields are source unit.
    /// Throws std::overflow_error for layout overflow.
    /// Complexity: O(payload size + field count) time and owned space.
    /// Example: `auto unit = value::tuple({});`.
    static value tuple(std::span<const value> fields);
    /// Copies a record of the specified nominal type.
    /// Throws std::invalid_argument for a non-record, wrong arity, or wrong field type.
    /// Failure releases earlier children without leaking resources.
    /// Complexity: O(payload size + field count) time and owned space.
    /// Example: `auto empty = value::record(value_type::record("sample", {}), {});`.
    static value record(value_type type, std::span<const value> fields);
    /// Copies endpoints in source order for the given range descriptor.
    /// Throws std::invalid_argument for a non-range, wrong arity, or wrong endpoint type.
    /// Complexity: O(endpoint byte size) time and owned space.
    /// Example: `auto full = value::range(value_type::range(
    ///     range_kind::range_full, value_type::leaf<double>()), {});`.
    static value range(value_type type, std::span<const value> endpoints);
    /// Returns array length, fixed aggregate arity, or range endpoint count.
    /// Throws std::invalid_argument for leaves or callables.
    /// Example: `value::tuple({}).size() == 0`.
    std::size_t size() const;
    /// Copies a source-order child independently.
    /// Throws std::out_of_range for an invalid index, std::invalid_argument for non-composites.
    /// Complexity: O(child payload size) time and owned result space.
    /// Example: `value::from(std::vector<double>{2.0}).at(0).as<double>() == 2.0`.
    value at(std::size_t index) const;
    /// Replaces one child with an independent copy of the exact expected shape.
    /// Throws std::out_of_range for invalid index, std::invalid_argument for shape mismatch
    /// or non-composites. Failure leaves the value unchanged; no rollback copy is needed.
    /// Complexity: O(old and new child payload size) time and O(new child size) new space.
    /// Example: `auto a = value::from(std::vector<double>{1.0}); a.set(0, value::from(2.0));`.
    void set(std::size_t index, const value& replacement);
    /// Copies a named record field; throws std::invalid_argument for non-record/missing name.
    /// Complexity: O(field count + name bytes + child payload size) time and child space.
    /// Example: `value::record(TYPE, FIELDS).field("enabled").as<bool>()`.
    value field(std::string_view name) const;
    /// Replaces a named record field with an independent, exact-shape copy.
    /// Throws std::invalid_argument for non-record/missing name or shape mismatch.
    /// Complexity: O(field count + name bytes + old/new child size) time and new child space.
    /// Example: `record.set_field("enabled", value::from(true));`.
    void set_field(std::string_view name, const value& replacement);
};

namespace detail {

/// Adapts only trusted concrete trivially copyable leaf types.
/// Leaf copies preserve bytes without inferring pointee ownership or semantic equality.
template <class T>
struct concrete_adapter {
    /// Returns the concrete type descriptor.
    static value_type type() { return value_type::leaf<T>(); }
    /// Copies a leaf into independently owned aligned bytes.
    /// Complexity: O(sizeof(T)) time and space.
    static value from(const T& source) { return value::from_leaf(type(), &source); }
    /// Extracts a leaf of the exact concrete identity; rejects other shapes.
    /// Complexity: O(sizeof(T)) time.
    static T as(const value& source) {
        std::array<std::byte, sizeof(T)> bytes{};
        source.copy_leaf(type(), bytes.data());
        return std::bit_cast<T>(bytes);
    }
    /// Extracts trusted canonical leaf bytes without creating an intermediate owner.
    /// Precondition: shape is type() and data contains its initialized representation.
    /// Complexity: O(sizeof(T)) time.
    static T read(const value_type&, const std::byte* data) {
        std::array<std::byte, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), data, bytes.size());
        return std::bit_cast<T>(bytes);
    }
};

/// Adapts an owning standard vector without registering its nontrivial facade as a leaf.
template <class T, class Allocator>
struct concrete_adapter<std::vector<T, Allocator>> {
    /// Returns the complete element shape, including for empty arrays.
    static value_type type() {
        static const auto descriptor = value_type::array(concrete_adapter<T>::type());
        return descriptor;
    }
    /// Copies all elements independently.
    /// Complexity: O(payload size + element count) time and space.
    static value from(const std::vector<T, Allocator>& source) {
        std::vector<value> children;
        children.reserve(source.size());
        // Materialize boundary values before transferring them into the owning array.
        for (const auto& child : source)
            children.push_back(value::from<T>(child));
        return value::array_owned(concrete_adapter<T>::type(), std::move(children));
    }
    /// Returns a disjoint vector; throws std::invalid_argument for the wrong complete shape.
    /// Complexity: O(payload size + element count) time and space.
    static std::vector<T, Allocator> as(const value& source) {
        if (!source.type().same_shape(type())) throw std::invalid_argument("wrong vector type");
        return read(source.type(), value::raw_data(source));
    }
    /// Extracts trusted canonical elements without intermediate payload clones.
    /// Precondition: shape is type() and data contains its initialized representation.
    /// Complexity: O(payload size + element count) time and result space.
    static std::vector<T, Allocator> read(const value_type&, const std::byte* data) {
        std::vector<T, Allocator> result;
        const auto children = value::array_children(data);
        result.reserve(children.size());
        // Extract each element through its trusted concrete adapter.
        for (const auto& child : children)
            result.push_back(concrete_adapter<T>::read(child.type(), value::raw_data(child)));
        return result;
    }
};

/// Adapts a standard tuple recursively, including source unit and singleton tuples.
template <class... T>
struct concrete_adapter<std::tuple<T...>> {
    /// Returns canonical source-order field shapes.
    /// Complexity: O(field count) time and metadata space.
    static value_type type() {
        static const auto descriptor = [] {
            const std::array<value_type, sizeof...(T)> fields{concrete_adapter<T>::type()...};
            return value_type::tuple(fields);
        }();
        return descriptor;
    }
    /// Copies tuple fields through trusted adapters.
    /// Complexity: O(payload size + field count) time and space.
    static value from(const std::tuple<T...>& source) {
        return std::apply(
            [](const auto&... child) {
                std::array<value, sizeof...(T)> fields{value::from(child)...};
                return value::tuple_owned(fields);
            },
            source);
    }
    /// Extracts source-order fields; the complete shape has already been checked.
    /// Complexity: O(payload size + field count) time and space.
    template <std::size_t... I>
    static std::tuple<T...> extract(const value_type& shape,
                                    const std::byte* data,
                                    std::index_sequence<I...>) {
        return {concrete_adapter<T>::read(shape.fields()[I],
                                          data ? data + shape.offsets()[I] : nullptr)...};
    }
    /// Returns a disjoint tuple; throws std::invalid_argument for the wrong complete shape.
    /// Complexity: O(payload size + field count) time and space.
    static std::tuple<T...> as(const value& source) {
        if (!source.type().same_shape(type())) throw std::invalid_argument("wrong tuple type");
        return read(source.type(), value::raw_data(source));
    }
    /// Extracts trusted canonical fields without intermediate payload clones.
    /// Precondition: shape is type() and data contains its initialized representation.
    /// Complexity: O(payload size + field count) time and result space.
    static std::tuple<T...> read(const value_type& shape, const std::byte* data) {
        return extract(shape, data, std::index_sequence_for<T...>{});
    }
};

} // namespace detail
} // namespace cel
#endif
