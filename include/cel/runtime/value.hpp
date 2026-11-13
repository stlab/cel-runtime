// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_VALUE_HPP
#define CEL_RUNTIME_VALUE_HPP

#include <cel/runtime/value_type.hpp>

#include <array>
#include <bit>
#include <cassert>
#include <cstring>
#include <memory>
#include <span>
#include <stdexcept>
#include <tuple>
#include <utility>
#include <vector>

namespace cel {
/// \addtogroup runtime_values
/// @{

/// Provides trusted concrete boundary adapters; these are not client-leaf registrations.
namespace detail {
struct owned_payload;
struct value_access;
/// A borrowed view of initialized packed array elements.
///
/// The owning value retains the element descriptor and bytes; mutation or retirement
/// of that owner invalidates this view.
struct array_view {
    const value_type& element; ///< Borrows the complete canonical element type.
    const std::byte* data;     ///< Borrows initialized canonical element bytes.
    std::size_t size;          ///< Counts initialized elements.
    std::size_t stride;        ///< Gives the canonically padded element byte extent.
};
template <class T>
struct concrete_adapter;
} // namespace detail

/// Holds any type representable by CEL, including client-defined types.
///
/// In the costs below, B counts canonical bytes copied or initialized, F visited fixed
/// fields or native elements, N visited array owners, and L sums ownership-slot selection
/// costs: O(sum of log(field count + 1) along each selected aggregate path).
/// F counts logical visits even when their byte extent is zero.
/// C counts opaque clone/destruction and released executable costs, including client
/// capture destruction; D counts released metadata nodes, edges,
/// and identity/name bytes. W = B + F + N + L + C + D is the affected payload work.
/// Owned result storage includes owner records and newly created metadata, not just byte
/// buffers. Result storage is separate from auxiliary space. Native adapters may use call space
/// proportional to concrete nesting; canonical cloning and retirement use no nesting-
/// dependent call space or allocated traversal storage beyond cloned result owners.
///
/// Example: `auto number = value::from(2.0); auto copy = number;`.
///
/// Create a record and change a copied field without changing the original:
/// @code
/// const auto fields =
///     std::array{cel::record_field{"enabled", cel::native_type<bool>()}};
/// const auto schema = cel::value_type::record("settings", fields);
/// auto original =
///     cel::value::record(schema, std::array{cel::value::from(false)});
/// auto copy = original;
/// copy.set_field("enabled", cel::value::from(true));
/// auto enabled = copy.field("enabled").as<bool>(); // true.
/// // original.field("enabled").as<bool>() remains false.
/// @endcode
// [knowledge] Never byte-relocate public wrappers; only private canonical carriers transfer owners.
class value {
    std::unique_ptr<detail::owned_payload> _payload;
    friend struct detail::value_access;
    template <class T>
    friend struct detail::concrete_adapter;

    /// Takes uniquely owned, fully initialized payload storage.
    ///
    /// \pre payload describes exactly one complete canonical owner, or is null for unit.
    explicit value(std::unique_ptr<detail::owned_payload> payload);
    /// Copies a trusted concrete leaf representation into aligned storage.
    ///
    /// \pre type describes the concrete trivially copyable leaf at source; source provides
    /// its readable extent.
    /// Complexity: O(type byte size) time and result storage.
    static value from_leaf(value_type type, const void* source);
    /// Copies a leaf into its trusted adapter.
    ///
    /// \pre expected describes a concrete leaf; destination provides its writable extent
    /// and is disjoint from the source representation.
    /// Throws std::invalid_argument on type mismatch, leaving destination unchanged.
    /// Complexity: O(type byte size + M) expected time and comparison auxiliary space,
    /// where M is the same_shape comparison cost.
    void copy_leaf(const value_type& expected, void* destination) const;
    /// Copies fields into an independently owned fixed aggregate.
    ///
    /// \pre type is a tuple, record, or range.
    /// Leaves inputs unchanged.
    /// Throws std::invalid_argument for wrong arity or incompatible shape/ownership operations.
    /// Failure destroys every initialized child without allocating rollback copies.
    /// Complexity: O(W + M) expected time and new owned result storage, where M is
    /// the sum of complete compatibility comparison costs; comparisons use auxiliary space.
    static value aggregate(value_type type, std::span<const value> fields);
    /// Takes already-owned boundary children without cloning their mutable payloads.
    ///
    /// Consumed children become unit.
    /// Throws std::invalid_argument for wrong shapes; failure releases completed transfers.
    /// Remaining sources retain ownership.
    /// Throws std::overflow_error for canonical extent overflow before consuming children.
    /// Complexity: O(B + F + M + D) expected time and packed result storage, with M
    /// the sum of compatibility comparison costs; failure also retires consumed payloads.
    static value array_owned(value_type element, std::span<value> children);
    /// Borrows a public owner's canonical bytes.
    ///
    /// The view expires when owner changes or dies; unit can have null bytes.
    static const std::byte* raw_data(const value& owner) noexcept;
    /// Borrows packed array elements from a live canonical carrier.
    ///
    /// \pre data contains an initialized array carrier and its owner remains alive and
    /// unchanged for the lifetime of the returned view.
    static detail::array_view array_data(const std::byte* data) noexcept;
    /// Returns an independently owned canonical value from a concrete source.
    ///
    /// \pre source and type match the writer's concrete representation; write initializes
    /// exactly one owner on success and cleans its partial construction if it throws.
    /// Failure publishes no result and releases outer storage.
    /// Complexity: O(canonical outer extent + writer cost) time and result storage;
    /// auxiliary space is the writer's requirement.
    static value from_concrete(value_type type,
                               const void* source,
                               void (*write)(const void*, const value_type&, std::byte*));
    /// Initializes a packed array carrier from concrete source elements.
    ///
    /// \pre destination is aligned, disjoint, uninitialized array-carrier storage.
    /// \pre write accepts each source index below count, initializes one canonical
    /// element matching element, and releases its partial element on failure.
    /// Failure retires completed elements and leaves destination uninitialized.
    /// Throws std::overflow_error if the element count overflows the canonical byte extent.
    /// Complexity: O(count times element extent + count + total writer/cleanup costs)
    /// time and result storage; auxiliary space is the writer's requirement.
    static void write_array(const value_type& element,
                            std::byte* destination,
                            std::size_t count,
                            const void* source,
                            void (*write)(const void*, std::size_t, const value_type&, std::byte*));
    /// Destroys an initialized tuple/record field prefix without allocating.
    ///
    /// \pre type is a tuple or record; count <= arity and exactly that prefix consists
    /// of valid owned representations in data.
    /// Complexity: O(F + N + L + C + D) time and O(1) runtime auxiliary space.
    static void destroy_fields(const value_type& type, std::byte* data, std::size_t count) noexcept;

public:
    /// Creates the unit value `()` without allocating.
    ///
    /// Example: `value unit;`.
    value() noexcept;
    /// Destroys owned children and releases storage without allocating.
    ///
    /// Runtime retirement uses no nesting-dependent call space; client capture destructors
    /// retain their own costs and behavior.
    /// Complexity: O(F + N + L + C + D) time and O(1) runtime auxiliary space.
    ///
    /// See the class's shared copying example.
    ~value();
    /// Copies the source under the class's aggregate and callable copy semantics.
    ///
    /// Leaves the source unchanged; failure releases every acquired owner.
    /// Complexity: O(W) time and new owned result storage, with O(1) runtime auxiliary
    /// space excluding opaque operations.
    ///
    /// See the class's shared copying example.
    value(const value& other);
    /// Transfers ownership and leaves other as source unit.
    ///
    /// Example: `auto owner = std::move(source); // source is unit.`
    value(value&& other) noexcept;
    /// Replaces this value with a copy under the class's aggregate and callable copy semantics.
    ///
    /// Leaves other unchanged; self-assignment leaves this value unchanged.
    /// Failure leaves this unchanged without copying its prior payload.
    /// Complexity: O(source/old W) time and new source-sized result storage; runtime
    /// auxiliary space is O(1), excluding opaque operations.
    ///
    /// Example: `auto target = value{}; target = source;`.
    value& operator=(const value& other);
    /// Releases old ownership, transfers other, and leaves other as unit; self-move is unchanged.
    ///
    /// Complexity: O(old F + N + L + C + D) time and O(1) runtime auxiliary space.
    ///
    /// Example: `auto target = value{}; target = std::move(source); // source is unit.`
    value& operator=(value&& other) noexcept;
    /// Borrows the complete descriptor until this value is assigned or destroyed.
    ///
    /// The first descriptor query on a default/moved-from value may allocate unit metadata.
    ///
    /// Example: `value::from(true).type().kind() == type_kind::leaf`.
    const value_type& type() const;
    /// Copies a concrete leaf or recursively nested owning vector/tuple into a runtime value.
    ///
    /// Leaves source unchanged; pointer members in opaque leaves do not confer pointee ownership.
    /// Complexity: O(B + F + first-use native_type construction) time and owned result
    /// storage; O(concrete nesting depth) adapter call space.
    ///
    /// Example: `auto numbers = value::from(std::vector<double>{1.0, 2.0});`.
    template <class T>
    static value from(const T& source) {
        return detail::concrete_adapter<std::remove_cv_t<T>>::from(source);
    }
    /// Returns a concrete copy of the exact requested leaf, vector, or tuple type.
    ///
    /// Returned vectors and tuples own independent mutable payloads; leaf pointers remain pointers.
    ///
    /// Throws std::invalid_argument if the complete concrete type does not match.
    /// Complexity: O(B + F + M + first-use native_type construction) expected time and
    /// owned result storage, where M is the same_shape comparison cost; comparison
    /// auxiliary space and O(concrete nesting depth) adapter call space are additional.
    ///
    /// Example: `value::from(2.0).as<double>() == 2.0`.
    template <class T>
    T as() const {
        return detail::concrete_adapter<std::remove_cv_t<T>>::as(*this);
    }
    /// Copies a sequence of elements with the specified complete shape and ownership operations.
    ///
    /// Retains element for an empty sequence; leaves all input values unchanged.
    ///
    /// Throws std::invalid_argument for incompatible elements, releasing earlier copies.
    /// Throws std::overflow_error if count times canonical element extent overflows.
    /// Complexity: O(W + M) expected time and owned result storage, where M sums
    /// element compatibility comparison costs; comparisons require auxiliary space.
    ///
    /// Example: `auto empty = value::array(value_type::leaf<double>(), {});`.
    static value array(value_type element, std::span<const value> elements);
    /// Copies source-order fields into a canonical tuple; empty fields are source unit.
    ///
    /// Leaves input fields unchanged, with the class's copy semantics for every component.
    ///
    /// Throws std::overflow_error for layout overflow.
    /// Complexity: O(W + field count) time and owned result storage; O(field count)
    /// temporary descriptor space, in addition to opaque operation costs.
    ///
    /// Example: `auto unit = value::tuple({});`.
    static value tuple(std::span<const value> fields);
    /// Copies source-order fields into a record of the specified nominal type.
    ///
    /// Leaves inputs unchanged, with the class's copy semantics for every component.
    ///
    /// Throws std::invalid_argument for a non-record, wrong arity, or incompatible fields.
    /// Field compatibility includes complete shape and ownership operations.
    /// Failure releases earlier children without leaking resources.
    /// Complexity: O(W + M) expected time and owned result storage, with M the sum of
    /// field compatibility comparison costs; comparisons require auxiliary space.
    ///
    /// Example: `auto empty = value::record(value_type::record("sample", {}), {});`.
    static value record(value_type type, std::span<const value> fields);
    /// Copies endpoints in source order for the given range descriptor.
    ///
    /// Leaves inputs unchanged.
    ///
    /// Throws std::invalid_argument for a non-range, wrong arity, or incompatible endpoint type.
    /// Complexity: O(endpoint byte size) time and result storage.
    ///
    /// Example: `auto full = value::range(value_type::range(
    ///     range_kind::range_full, value_type::leaf<double>()), {});`.
    static value range(value_type type, std::span<const value> endpoints);
    /// Returns array length, fixed aggregate arity, or range endpoint count.
    ///
    /// Throws std::invalid_argument for leaves or callables.
    ///
    /// Example: `value::tuple({}).size() == 0`.
    std::size_t size() const;
    /// Copies a source-order child under the class's aggregate and callable copy semantics.
    ///
    /// Leaves this value unchanged, including on failure.
    ///
    /// Throws std::out_of_range for an invalid index, std::invalid_argument for non-composites.
    /// Complexity: O(child W) time and owned result storage; O(1) runtime auxiliary
    /// space excluding opaque operations.
    ///
    /// Example: `value::from(std::vector<double>{2.0}).at(0).as<double>() == 2.0`.
    value at(std::size_t index) const;
    /// Replaces one child with a copy of the exact expected shape and ownership operations.
    ///
    /// Leaves replacement unchanged; callable children retain reference-semantic code.
    ///
    /// Throws std::out_of_range for invalid index, std::invalid_argument for shape mismatch
    /// or non-composites. Failure leaves the value unchanged; no rollback copy is needed.
    /// Complexity: O(old/new child W + M) expected time and new child result storage,
    /// where M is the compatibility comparison cost; comparison auxiliary space is additional.
    ///
    /// Example: `auto a = value::from(std::vector<double>{1.0}); a.set(0, value::from(2.0));`.
    void set(std::size_t index, const value& replacement);
    /// Copies a named record field under the class's copy semantics.
    ///
    /// Leaves this value unchanged, including on failure.
    /// Throws std::invalid_argument for a non-record or missing name.
    /// Complexity: linear field-name lookup, including compared name bytes, plus O(child W)
    /// copying time and child result storage; O(1) runtime auxiliary space excluding opaque
    /// operations.
    ///
    /// See the class example for a complete record and field lookup.
    value field(std::string_view name) const;
    /// Replaces a named record field with a copy of the exact expected shape.
    ///
    /// Throws std::invalid_argument for non-record/missing name or shape mismatch.
    /// Follows set's ownership compatibility, copy semantics, and unchanged-on-failure guarantee.
    /// Complexity: linear field-name lookup, including compared name bytes, plus
    /// O(old/new child W + M) expected replacement time and new child result storage,
    /// where M is compatibility comparison cost; comparison auxiliary space is additional.
    ///
    /// See the class example for a complete record and field replacement.
    void set_field(std::string_view name, const value& replacement);
};

namespace detail {

/// A concrete boundary adapter for trivially copyable leaves.
///
/// Leaf copies preserve bytes without inferring pointee ownership or semantic equality.
template <class T>
struct concrete_adapter {
    /// Returns the concrete type descriptor.
    static value_type type() { return value_type::leaf<T>(); }
    /// Copies a leaf into independently owned aligned bytes.
    ///
    /// Complexity: O(sizeof(T)) time and result storage.
    static value from(const T& source) { return value::from_leaf(type(), &source); }
    /// Extracts a leaf of the exact concrete identity.
    ///
    /// Throws std::invalid_argument for another identity or shape.
    /// Complexity: O(sizeof(T)) time.
    static T as(const value& source) {
        std::array<std::byte, sizeof(T)> bytes{};
        source.copy_leaf(type(), bytes.data());
        return std::bit_cast<T>(bytes);
    }
    /// Returns a concrete leaf copied from canonical bytes.
    ///
    /// Creates no intermediate public owner.
    /// \pre The supplied descriptor is type(); data provides its readable initialized
    /// concrete representation.
    /// Complexity: O(sizeof(T)) time.
    static T read(const value_type&, const std::byte* data) {
        assert(data != nullptr);
        std::array<std::byte, sizeof(T)> bytes{};
        std::memcpy(bytes.data(), data, bytes.size());
        return std::bit_cast<T>(bytes);
    }
    /// Writes a trusted leaf directly into uninitialized canonical bytes.
    ///
    /// \pre The supplied descriptor is type(); data provides its uninitialized writable
    /// extent, disjoint from source.
    /// Complexity: O(sizeof(T)) time.
    static void write(const T& source, const value_type&, std::byte* data) {
        static_assert(std::is_trivially_copyable_v<T>);
        std::memcpy(data, &source, sizeof(T));
    }
};

/// A recursive concrete boundary adapter for an owning standard vector.
///
/// Canonical copies own independent elements and do not byte-relocate the native facade.
template <class T, class Allocator>
struct concrete_adapter<std::vector<T, Allocator>> {
    /// Returns the complete array descriptor, including its element type when empty.
    ///
    /// Complexity: first use includes element descriptor construction; subsequent calls
    /// take O(1) time and auxiliary space.
    static value_type type() {
        static const auto descriptor = value_type::array(concrete_adapter<T>::type());
        return descriptor;
    }
    /// Copies all elements independently.
    ///
    /// Leaves source unchanged; failure publishes no owner.
    /// Complexity: O(B + F + first-use type construction) time and result storage,
    /// with B and F as in value; O(concrete nesting depth) call space.
    static value from(const std::vector<T, Allocator>& source) {
        return value::from_concrete(
            type(), &source, [](const void* object, const value_type& shape, std::byte* data) {
                write(*static_cast<const std::vector<T, Allocator>*>(object), shape, data);
            });
    }
    /// Initializes a canonical array from independently copied vector elements.
    ///
    /// \pre shape is type(); data is aligned uninitialized array-carrier storage,
    /// disjoint from source.
    /// Failure releases completed elements and leaves data uninitialized.
    /// Complexity: O(B + F) time and result storage, with B and F as in value;
    /// O(concrete nesting depth) call space.
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
    /// Returns a vector with independently owned elements.
    ///
    /// Throws std::invalid_argument for the wrong complete shape; leaves source unchanged.
    /// Complexity: O(B + F + M + first-use type construction) expected time and result
    /// storage, with B and F as in value and M the shape comparison cost;
    /// comparison auxiliary space and O(concrete nesting depth) call space are additional.
    static std::vector<T, Allocator> as(const value& source) {
        if (!source.type().same_shape(type())) throw std::invalid_argument("wrong vector type");
        return read(source.type(), value::raw_data(source));
    }
    /// Returns an independently owned vector from canonical bytes.
    ///
    /// Performs no intermediate payload cloning.
    /// \pre The supplied descriptor is type(); data and its owned descendants remain
    /// valid and unchanged throughout extraction.
    /// Returns independently owned elements; failure releases the partial result.
    /// Complexity: O(B + F) time and result storage, with B and F as in value;
    /// O(concrete nesting depth) call space.
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

/// A recursive concrete boundary adapter for standard tuples, including unit and singletons.
template <class... T>
struct concrete_adapter<std::tuple<T...>> {
    /// Returns canonical source-order field shapes.
    ///
    /// Complexity: first use includes field descriptor construction and O(field count)
    /// time and result metadata; subsequent calls take O(1) time and auxiliary space.
    static value_type type() {
        static const auto descriptor = [] {
            const std::array<value_type, sizeof...(T)> fields{concrete_adapter<T>::type()...};
            return value_type::tuple(fields);
        }();
        return descriptor;
    }
    /// Returns an independently owned canonical tuple.
    ///
    /// Leaves source unchanged; failure publishes no owner.
    /// Complexity: O(B + F + first-use type construction) time and result storage,
    /// with B and F as in value; O(concrete nesting depth) call space.
    static value from(const std::tuple<T...>& source) {
        return value::from_concrete(
            type(), &source, [](const void* object, const value_type& shape, std::byte* data) {
                write(*static_cast<const std::tuple<T...>*>(object), shape, data);
            });
    }
    /// Initializes independently copied canonical fields in source order.
    ///
    /// \pre shape is type(); data is aligned uninitialized tuple storage disjoint from
    /// source, and I selects all fields in source order.
    /// Failure destroys completed fields and leaves data uninitialized.
    /// Complexity: O(B + F) time and result storage, with B and F as in value;
    /// O(concrete nesting depth) call space.
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
    /// Initializes a canonical tuple from independently copied native fields.
    ///
    /// \pre shape is type(); data is aligned uninitialized tuple storage disjoint from source.
    /// Failure destroys completed fields and leaves data uninitialized.
    /// Complexity: O(B + F) time and result storage, with B and F as in value;
    /// O(concrete nesting depth) call space.
    static void write(const std::tuple<T...>& source, const value_type& shape, std::byte* data) {
        write_fields(source, shape, data, std::index_sequence_for<T...>{});
    }
    /// Returns independently owned concrete fields in source order.
    ///
    /// \pre shape is type(); data and its owned descendants remain valid and unchanged
    /// throughout extraction, and I selects all fields in source order.
    /// Failure releases the partial result.
    /// Complexity: O(B + F) time and result storage, with B and F as in value;
    /// O(concrete nesting depth) call space.
    template <std::size_t... I>
    static std::tuple<T...> extract(const value_type& shape,
                                    const std::byte* data,
                                    std::index_sequence<I...>) {
        return {concrete_adapter<T>::read(shape.fields()[I],
                                          data ? data + shape.offsets()[I] : nullptr)...};
    }
    /// Returns a tuple with independently owned fields.
    ///
    /// Throws std::invalid_argument for the wrong complete shape; leaves source unchanged.
    /// Complexity: O(B + F + M + first-use type construction) expected time and result
    /// storage, with B and F as in value and M the shape comparison cost;
    /// comparison auxiliary space and O(concrete nesting depth) call space are additional.
    static std::tuple<T...> as(const value& source) {
        if (!source.type().same_shape(type())) throw std::invalid_argument("wrong tuple type");
        return read(source.type(), value::raw_data(source));
    }
    /// Returns an independently owned tuple from canonical bytes.
    ///
    /// Performs no intermediate payload cloning.
    /// \pre shape is type(); data and its owned descendants remain valid and unchanged
    /// throughout extraction.
    /// Returns independently owned fields; failure releases the partial result.
    /// Complexity: O(B + F) time and result storage, with B and F as in value;
    /// O(concrete nesting depth) call space.
    static std::tuple<T...> read(const value_type& shape, const std::byte* data) {
        return extract(shape, data, std::index_sequence_for<T...>{});
    }
};

} // namespace detail
/// Returns the runtime's complete descriptor for a trusted native value type.
///
/// Tuples and vectors use canonical structural adapters, not native object layouts.
/// Complexity: O(V + E) time and result metadata on first use, with V distinct concrete
/// descriptor types and E their field/element edges; O(concrete nesting depth) call space.
/// Subsequent calls take O(1) time and auxiliary space.
///
/// Example: `auto type = native_type<std::vector<std::tuple<bool, double>>>();`.
template <class T>
value_type native_type() {
    return detail::concrete_adapter<std::remove_cvref_t<T>>::type();
}

/// @}
} // namespace cel
#endif
