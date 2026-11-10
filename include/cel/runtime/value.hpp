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
/// Borrows packed array elements; the owning value controls the view's lifetime.
struct array_view {
    const value_type& element; ///< Borrows the complete canonical element type.
    const std::byte* data;     ///< Borrows initialized canonical element bytes.
    std::size_t size;          ///< Counts initialized elements.
    std::size_t stride;        ///< Gives the canonically padded element byte extent.
};
template <class T>
struct concrete_adapter;
} // namespace detail

/// Owns a complete runtime value; copies own independent mutable payloads.
/// Installed callables are explicit reference-semantic code handles: copies share frozen code,
/// callback references, and the carrier's recursive-entry guard, not mutable aggregate payloads.
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
    /// Throws std::invalid_argument for wrong shapes; failure releases completed transfers.
    /// Remaining sources retain ownership.
    /// Complexity: O(canonical element bytes + compared shape size) time and packed outer storage.
    static value array_owned(value_type element, std::span<value> children);
    /// Borrows canonical bytes from a live public owner.
    static const std::byte* raw_data(const value& owner) noexcept;
    /// Borrows packed array elements from a live canonical carrier.
    /// Precondition: data contains an initialized array carrier.
    static detail::array_view array_data(const std::byte* data) noexcept;
    /// Initializes one canonical owner through a trusted concrete boundary writer.
    /// The writer cleans up partial construction if it throws.
    /// Complexity: O(payload size) time and owned space.
    static value from_concrete(value_type type,
                               const void* source,
                               void (*write)(const void*, const value_type&, std::byte*));
    /// Writes a packed array with one element allocation and tracked initialized prefix.
    /// The element writer releases its partial element on failure.
    /// Throws std::overflow_error if the element count overflows the canonical byte extent.
    /// Complexity: O(payload size + element count) time and owned space.
    static void write_array(const value_type& element,
                            std::byte* destination,
                            std::size_t count,
                            const void* source,
                            void (*write)(const void*, std::size_t, const value_type&, std::byte*));
    /// Destroys an initialized tuple/record field prefix without allocating.
    /// Precondition: count <= arity and exactly that prefix is initialized.
    /// Complexity: O(visited ownership paths + owned descendants) time and O(1) auxiliary space.
    static void destroy_fields(const value_type& type, std::byte* data, std::size_t count) noexcept;

public:
    /// Creates source unit without allocating payload storage.
    value() noexcept;
    /// Destroys owned children and releases storage without allocating.
    /// Complexity: O(visited canonical ownership paths, array nodes, and released metadata)
    /// time and O(1) auxiliary space; destruction does not recurse with nesting depth.
    ~value();
    /// Copies mutable aggregate payloads independently; callable code handles retain reference
    /// semantics. Complexity: O(canonical payload bytes + visited ownership paths) time and owned
    /// space.
    value(const value& other);
    /// Transfers ownership and leaves other as source unit.
    value(value&& other) noexcept;
    /// Replaces ownership with an independent copy, including self-assignment.
    /// Failure leaves this unchanged without copying its prior payload.
    /// Complexity: O(canonical source bytes + visited source/old ownership paths) time
    /// and O(source payload size) new owned space.
    value& operator=(const value& other);
    /// Releases old ownership, transfers other, and leaves other as unit; self-move is unchanged.
    /// Complexity: O(visited old ownership paths, array nodes, and released metadata) time.
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
    /// Throws std::overflow_error if count times canonical element extent overflows.
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
    /// Writes a trusted leaf directly into uninitialized canonical bytes.
    /// Complexity: O(sizeof(T)) time.
    static void write(const T& source, const value_type&, std::byte* data) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::memcpy(data, &source, sizeof(T));
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
        return value::from_concrete(
            type(), &source, [](const void* object, const value_type& shape, std::byte* data) {
                write(*static_cast<const std::vector<T, Allocator>*>(object), shape, data);
            });
    }
    /// Writes packed canonical elements without constructing public scalar boxes.
    /// The initialized prefix owns its elements and is destroyed on failure.
    /// Complexity: O(payload size + element count) time and owned space.
    static void write(const std::vector<T, Allocator>& source,
                      const value_type& shape,
                      std::byte* data) {
        value::write_array(shape.element(), data, source.size(), &source,
                           [](const void* object, std::size_t index, const value_type& element,
                              std::byte* target) {
                               const auto& input =
                                   *static_cast<const std::vector<T, Allocator>*>(object);
                               concrete_adapter<T>::write(input[index], element, target);
                           });
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
        const auto children = value::array_data(data);
        result.reserve(children.size);
        // Extract each element through its trusted concrete adapter.
        for (std::size_t i = 0; i != children.size; ++i)
            result.push_back(concrete_adapter<T>::read(
                children.element, children.data ? children.data + i * children.stride : nullptr));
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
        return value::from_concrete(
            type(), &source, [](const void* object, const value_type& shape, std::byte* data) {
                write(*static_cast<const std::tuple<T...>*>(object), shape, data);
            });
    }
    /// Writes canonical fields in source order and destroys its prefix on failure.
    /// Complexity: O(payload size + field count) time and owned space.
    template <std::size_t... I>
    static void write_fields(const std::tuple<T...>& source,
                             const value_type& shape,
                             std::byte* data,
                             std::index_sequence<I...>) {
        std::size_t completed = 0;
        try {
            ((concrete_adapter<T>::write(std::get<I>(source), shape.fields()[I],
                                         data ? data + shape.offsets()[I] : nullptr),
              ++completed),
             ...);
        } catch (...) {
            value::destroy_fields(shape, data, completed);
            throw;
        }
    }
    /// Writes a tuple facade without byte-relocating it or creating scalar owners.
    /// Complexity: O(payload size + field count) time and owned space.
    static void write(const std::tuple<T...>& source, const value_type& shape, std::byte* data) {
        write_fields(source, shape, data, std::index_sequence_for<T...>{});
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
/// Returns the runtime's complete descriptor for a trusted native value type.
/// Removes cv/reference qualifiers; leaves must be trivially copyable.
/// Tuples and vectors use canonical structural adapters, not native object layouts.
/// Complexity: O(distinct descriptor nodes and fields) on first use, O(1) thereafter.
/// Example: `auto type = native_type<std::vector<std::tuple<bool, double>>>();`.
template <class T>
value_type native_type() {
    return detail::concrete_adapter<std::remove_cvref_t<T>>::type();
}
} // namespace cel
#endif
