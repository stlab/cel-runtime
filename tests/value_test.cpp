// SPDX-License-Identifier: BSL-1.0
#include "storage.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <cel/runtime/value.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <tuple>
#include <vector>

/// Exercises concrete identity independently of layout.
struct alignas(256) aligned_leaf {
    /// Stores a client scalar.
    std::uint64_t number;
};

/// Supplies another client type with the same representation.
struct other_leaf {
    /// Stores a binary64 scalar.
    double number;
};

/// Requests a test callback failure after acquiring a temporary resource.
bool fail_test_clone = false;
/// Counts live resources created by the synthetic opaque clone callback.
int live_test_owners = 0;

/// Clones a synthetic opaque owner without implementing a callable object.
void clone_test_owner(const std::byte* source, std::byte* destination) {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), source, sizeof(pointer));
    auto copy = std::make_unique<int>(*pointer);
    if (fail_test_clone) throw std::runtime_error("synthetic clone failure");
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
        const auto live_before = live_test_owners;
        auto destination = cel::value::from(false);
        fail_test_clone = true;
        CHECK_THROWS_AS(cel::value::record(failing_record, children), std::runtime_error);
        CHECK_THROWS_AS(cel::value{good_record}, std::runtime_error);
        CHECK_THROWS_AS(destination = good_record, std::runtime_error);
        fail_test_clone = false;
        CHECK(live_test_owners == live_before);
        CHECK_FALSE(destination.as<bool>());
    }
    CHECK(live_test_owners == 0);
}

/// Catches lost scalar bits, insufficient alignment, and layout-based casts.
TEST_CASE("concrete adapters preserve scalar values and reject wrong identities") {
    CHECK(cel::value::from(true).as<bool>());
    CHECK(cel::value::from(4.5).as<double>() == 4.5);
    CHECK(cel::value::from(aligned_leaf{42}).as<aligned_leaf>().number == 42);
    CHECK(cel::value_type::leaf<aligned_leaf>().byte_layout().alignment == 256);
    CHECK_THROWS_AS(cel::value::from(4.5).as<other_leaf>(), std::invalid_argument);
    CHECK_THROWS_AS(cel::value::from(true).as<double>(), std::invalid_argument);
    const auto nan = cel::value::from(std::numeric_limits<double>::quiet_NaN()).as<double>();
    CHECK(std::isnan(nan));
}

/// Catches conflation of empty array, source unit, and singleton tuple.
TEST_CASE("empty and nested owning boundary adapters retain complete shapes") {
    auto empty = cel::value::from(std::vector<double>{});
    CHECK(empty.size() == 0);
    CHECK(empty.as<std::vector<double>>().empty());
    CHECK(empty.type().element().same_shape(cel::value_type::leaf<double>()));
    auto unit = cel::value::from(std::tuple<>{});
    CHECK(unit.size() == 0);
    CHECK(unit.type().byte_layout().size == 0);
    CHECK_FALSE(unit.type().same_shape(empty.type()));
    CHECK(unit.as<std::tuple<>>() == std::tuple<>{});
    const auto nested = std::tuple{std::tuple{true}, 3.0};
    CHECK(cel::value::from(nested).as<decltype(nested)>() == nested);
    CHECK_FALSE(
        cel::value::from(std::tuple{true}).type().same_shape(cel::value_type::leaf<bool>()));
    using pair = std::tuple<bool, double>;
    const std::vector<pair> pairs{{true, 2.0}, {false, 5.0}};
    CHECK(cel::value::from(pairs).as<std::vector<pair>>() == pairs);
    const std::vector<bool> flags{true, false, true};
    CHECK(cel::value::from(flags).as<std::vector<bool>>() == flags);
    const auto over_aligned = std::tuple{true, aligned_leaf{73}, std::vector<double>{6.0}};
    const auto aligned_value = cel::value::from(over_aligned);
    CHECK(std::get<1>(aligned_value.as<decltype(over_aligned)>()).number == 73);
    CHECK(aligned_value.type().offsets()[1] == 256);
    CHECK(aligned_value.type().byte_layout().alignment == 256);
    const auto nested_arrays = std::vector{std::tuple{std::vector<double>{1.0, 2.0}, flags}};
    CHECK(cel::value::from(nested_arrays).as<decltype(nested_arrays)>() == nested_arrays);
    CHECK_THROWS_AS(empty.at(0), std::out_of_range);
    CHECK_THROWS_AS(cel::value::from(true).size(), std::invalid_argument);
}

/// Catches omission of field types, canonical padding, and nominal identities.
TEST_CASE("descriptors distinguish complete shapes rather than equal layouts") {
    using cel::value_type;
    const auto boolean = value_type::leaf<bool>();
    const auto number = value_type::leaf<double>();
    CHECK_FALSE(number.same_shape(value_type::leaf<other_leaf>()));
    CHECK_FALSE(value_type::array(number).same_shape(value_type::array(boolean)));
    const std::array fields{boolean, number, boolean};
    const auto tuple = value_type::tuple(fields);
    CHECK(tuple.offsets()[0] == 0);
    CHECK(tuple.offsets()[1] == 8);
    CHECK(tuple.offsets()[2] == 16);
    CHECK(tuple.byte_layout().size == 24);
    CHECK(tuple.byte_layout().alignment == 8);
    const std::array reverse{number, boolean, boolean};
    CHECK_FALSE(tuple.same_shape(value_type::tuple(reverse)));
    CHECK_FALSE(
        value_type::tuple(std::array{number, boolean})
            .same_shape(value_type::tuple(std::array{value_type::leaf<other_leaf>(), boolean})));
    const std::array record_fields{cel::record_field{"items", value_type::array(number)}};
    const auto first = value_type::record("first", record_fields);
    CHECK(first.record_identity() == "first");
    CHECK(first.field_names()[0] == "items");
    CHECK(first.same_shape(value_type(first)));
    CHECK_FALSE(first.same_shape(value_type::record("second", record_fields)));
    CHECK_FALSE(first.same_shape(value_type::tuple(std::array{value_type::array(number)})));
    CHECK_THROWS_AS(value_type::record("", record_fields), std::invalid_argument);
    CHECK_THROWS_AS(value_type::record("duplicate", std::array{record_fields[0], record_fields[0]}),
                    std::invalid_argument);
    CHECK_THROWS_AS(number.element(), std::invalid_argument);
    CHECK_THROWS_AS(number.record_identity(), std::invalid_argument);
    CHECK_THROWS_AS(number.fields(), std::invalid_argument);
    CHECK_THROWS_AS(number.offsets(), std::invalid_argument);
    CHECK_THROWS_AS(number.field_names(), std::invalid_argument);
    CHECK_THROWS_AS(tuple.leaf_identity(), std::invalid_argument);
    CHECK_THROWS_AS(value_type::record("empty-name", std::array{cel::record_field{"", boolean}}),
                    std::invalid_argument);
    const auto renamed = std::array{cel::record_field{"other", value_type::array(number)}};
    CHECK_FALSE(first.same_shape(value_type::record("first", renamed)));
}

/// Catches shallow copies of arrays nested inside tuples and records.
TEST_CASE("record and tuple copies and assignments own disjoint mutable payloads") {
    const auto items = cel::value::from(std::vector<double>{1.0, 2.0});
    const auto record =
        cel::value_type::record("sample", std::array{cel::record_field{"items", items.type()}});
    auto original = cel::value::record(record, std::array{items});
    auto copy = original;
    auto changed = original.field("items");
    changed.set(0, cel::value::from(9.0));
    original.set_field("items", changed);
    CHECK(copy.field("items").at(0).as<double>() == 1.0);
    changed = copy.field("items");
    changed.set(1, cel::value::from(7.0));
    copy.set_field("items", changed);
    CHECK(original.field("items").at(1).as<double>() == 2.0);
    auto assigned = cel::value::from(false);
    assigned = copy;
    copy.set_field("items", items);
    CHECK(assigned.field("items").at(1).as<double>() == 7.0);
    auto& alias = assigned;
    assigned = alias;
    CHECK(assigned.field("items").at(1).as<double>() == 7.0);
    auto tuple = cel::value::tuple(std::array{assigned});
    auto tuple_copy = tuple;
    tuple.set(0, original);
    CHECK(tuple_copy.at(0).field("items").at(1).as<double>() == 7.0);
    auto extracted = tuple_copy.at(0).field("items").as<std::vector<double>>();
    extracted[0] = 123.0;
    CHECK(tuple_copy.at(0).field("items").at(0).as<double>() == 1.0);
    CHECK_THROWS_AS(original.field("missing"), std::invalid_argument);
    CHECK_THROWS_AS(original.set_field("items", cel::value::from(true)), std::invalid_argument);
    CHECK_THROWS_AS(changed.set(5, cel::value::from(0.0)), std::out_of_range);
    CHECK_THROWS_AS(changed.set(0, cel::value::from(false)), std::invalid_argument);
}

/// Catches erasure of range form or endpoint identity.
TEST_CASE("all range forms retain their endpoint types and values") {
    const std::array forms{cel::range_kind::range,           cel::range_kind::range_from,
                           cel::range_kind::range_to,        cel::range_kind::range_to_inclusive,
                           cel::range_kind::range_inclusive, cel::range_kind::range_full};
    const std::array<std::size_t, 6> counts{2, 1, 1, 1, 2, 0};
    const std::array endpoints{cel::value::from(2.0), cel::value::from(5.0)};
    for (std::size_t i = 0; i != forms.size(); ++i) {
        const auto type = cel::value_type::range(forms[i], cel::value_type::leaf<double>());
        const auto range = cel::value::range(type, std::span(endpoints).first(counts[i]));
        CHECK(range.size() == counts[i]);
        CHECK(range.type().form() == forms[i]);
        CHECK(range.type().element().same_shape(cel::value_type::leaf<double>()));
        if (counts[i] != 0) CHECK(range.at(0).as<double>() == 2.0);
        CHECK_FALSE(
            type.same_shape(cel::value_type::range(forms[i], cel::value_type::leaf<bool>())));
    }
    CHECK_FALSE(cel::value_type::range(forms[1], cel::value_type::leaf<double>())
                    .same_shape(cel::value_type::range(forms[2], cel::value_type::leaf<double>())));
    CHECK_THROWS_AS(cel::value_type::range(cel::range_kind::range,
                                           cel::value_type::array(cel::value_type::leaf<double>())),
                    std::invalid_argument);
    CHECK_THROWS_AS(
        cel::value_type::range(static_cast<cel::range_kind>(99), cel::value_type::leaf<double>()),
        std::invalid_argument);
}

/// Catches incomplete callable result types and unsupported parameter admission.
TEST_CASE("callable descriptors retain complete signatures and validate capabilities") {
    const auto number = cel::value_type::leaf<double>();
    const auto boolean = cel::value_type::leaf<bool>();
    const auto client = cel::value_type::leaf<other_leaf>();
    const std::array parameters{cel::value_type::array(number),
                                cel::value_type::tuple(std::array{boolean})};
    const auto inner = cel::value_type::callable({}, client);
    const auto callable = cel::value_type::callable(parameters, inner);
    CHECK(callable.parameters().size() == 2);
    CHECK(callable.parameters()[0].same_shape(parameters[0]));
    CHECK(callable.result().same_shape(inner));
    CHECK(inner.result().same_shape(client));
    CHECK_FALSE(callable.owns_values());
    CHECK_FALSE(callable.same_shape(cel::value_type::callable(parameters, number)));
    CHECK_FALSE(callable.same_shape(cel::value_type::callable(std::array{boolean}, inner)));
    CHECK_FALSE(inner.same_shape(client));
    CHECK_THROWS_AS(cel::value_type::callable(std::array{client}, number), std::invalid_argument);
    CHECK_THROWS_AS(cel::value_type::callable(std::array{cel::value_type::array(client)}, number),
                    std::invalid_argument);
    CHECK_THROWS_AS(number.parameters(), std::invalid_argument);
    CHECK_THROWS_AS(number.result(), std::invalid_argument);
    CHECK_THROWS_AS(number.form(), std::invalid_argument);
    CHECK(number.owns_values());
    CHECK(cel::value_type::array(number).owns_values());
    CHECK_FALSE(cel::value_type::tuple(std::array{inner}).owns_values());
    CHECK(cel::value::array(inner, {}).size() == 0);
}

/// Catches leaked earlier children when a later child has a malformed type.
TEST_CASE("partial composite construction cleans up already owned children") {
    const auto numbers = cel::value::from(std::vector<double>{1.0, 2.0, 3.0});
    const auto record =
        cel::value_type::record("two", std::array{cel::record_field{"a", numbers.type()},
                                                  cel::record_field{"b", numbers.type()}});
    for (int i = 0; i != 100; ++i) {
        CHECK_THROWS_AS(cel::value::record(record, std::array{numbers, cel::value::from(false)}),
                        std::invalid_argument);
        CHECK_THROWS_AS(
            cel::value::array(numbers.type(), std::array{numbers, cel::value::from(false)}),
            std::invalid_argument);
    }

    CHECK(numbers.at(2).as<double>() == 3.0);
    CHECK_THROWS_AS(cel::value::record(record, std::array{numbers}), std::invalid_argument);
    CHECK_THROWS_AS(cel::value::record(numbers.type(), {}), std::invalid_argument);
    CHECK_THROWS_AS(cel::value::range(cel::value_type::range(cel::range_kind::range,
                                                             cel::value_type::leaf<double>()),
                                      std::array{cel::value::from(2.0)}),
                    std::invalid_argument);
}

/// Catches lost type identity during descriptor moves and lost unit state during value moves.
TEST_CASE("moves transfer values and leave valid source descriptors and unit values") {
    auto descriptor = cel::value_type::array(cel::value_type::leaf<double>());
    auto moved_type = std::move(descriptor);
    // Descriptor moves explicitly preserve a valid source.
    CHECK(descriptor.same_shape(moved_type)); // NOLINT(bugprone-use-after-move)
    cel::value_type assigned_type;
    assigned_type = std::move(moved_type);
    CHECK(assigned_type.same_shape(moved_type)); // NOLINT(bugprone-use-after-move)
    CHECK(assigned_type.element().same_shape(cel::value_type::leaf<double>()));
    auto source = cel::value::from(std::vector<double>{1.0});
    auto moved = std::move(source);
    // Value moves explicitly reset the source to usable source unit.
    CHECK(source.type().same_shape(cel::value_type{})); // NOLINT(bugprone-use-after-move)
    CHECK(source.size() == 0);
    CHECK(moved.at(0).as<double>() == 1.0);
    auto assigned = cel::value::from(false);
    assigned = std::move(moved);
    CHECK(moved.size() == 0); // NOLINT(bugprone-use-after-move)
    CHECK(assigned.at(0).as<double>() == 1.0);
    auto& alias = assigned;
    assigned = std::move(alias);
    CHECK(assigned.at(0).as<double>() == 1.0);
    CHECK(cel::value(source).size() == 0);
    assigned = source;
    CHECK(assigned.size() == 0);
}

/// Catches unchecked canonical aggregate addition and padding overflow.
TEST_CASE("canonical descriptor layout reports overflow without huge payload allocations") {
    auto type = cel::value_type::leaf<double>();
    while (type.byte_layout().size <= std::numeric_limits<std::size_t>::max() / 2) {
        type = cel::value_type::tuple(std::array{type, type});
    }
    CHECK_THROWS_AS(cel::value_type::tuple(std::array{type, type}), std::overflow_error);
}

/// Catches omitted complete result categories or capability propagation through records.
TEST_CASE("callable results retain every valid result category") {
    const auto number = cel::value_type::leaf<double>();
    const auto boolean = cel::value_type::leaf<bool>();
    const auto record =
        cel::value_type::record("result", std::array{cel::record_field{"flag", boolean}});
    const std::array results{number,
                             cel::value_type::leaf<other_leaf>(),
                             cel::value_type::array(number),
                             cel::value_type::tuple(std::array{number, boolean}),
                             record,
                             cel::value_type::range(cel::range_kind::range_full, number),
                             cel::value_type::callable({}, boolean)};
    for (const auto& result : results) {
        const auto callable = cel::value_type::callable(std::array{record}, result);
        CHECK(callable.result().same_shape(result));
        CHECK(callable.parameters()[0].same_shape(record));
    }
    const auto deferred =
        cel::value_type::record("closure", std::array{cel::record_field{"code", results.back()}});
    CHECK_FALSE(deferred.owns_values());
}

/// Owns a raw clone solely to verify private raw ownership contracts.
struct raw_owner {
    cel::value_type type;       ///< Retains immutable destruction metadata.
    cel::detail::storage bytes; ///< Owns aligned outer storage.
    bool initialized = false;   ///< Tracks whether payload destruction is required.
    /// Copies a public value into raw canonical storage.
    /// Complexity: O(payload size) time and space.
    explicit raw_owner(const cel::value& source) : type(source.type()) {
        const auto layout = type.byte_layout();
        bytes.reserve(layout.size, layout.alignment);
        bytes.resize(layout.size);
        cel::detail::clone_payload(type, cel::detail::value_access::data(source), bytes.data());
        initialized = true;
    }
    /// Destroys exactly one raw payload without allocating.
    /// Complexity: O(payload size) time and O(shape depth) call space.
    ~raw_owner() {
        if (initialized) cel::detail::destroy_payload(type, bytes.data());
    }
};

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
