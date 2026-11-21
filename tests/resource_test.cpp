// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/value.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <algorithm>
#include <array>
#include <atomic>
#include <doctest/doctest.h>
#include <latch>
#include <limits>
#include <memory>
#include <stdexcept>
#include <thread>
#include <vector>

/// Runs two nonthrowing callbacks on separate threads behind one start gate and joins them.
///
/// Thread-start failures propagate after releasing any already started worker.
/// Complexity: callback work plus thread startup and joining; O(1) auxiliary space.
template <class First, class Second>
void run_concurrently(First first, Second second) {
    std::latch start{1};
    std::array<std::jthread, 2> threads;
    const auto release = [](std::latch* gate) noexcept { gate->count_down(); };
    const std::unique_ptr<std::latch, decltype(release)> release_on_exit(&start, release);
    threads[0] = std::jthread([&] {
        start.wait();
        first();
    });
    threads[1] = std::jthread([&] {
        start.wait();
        second();
    });
}
/// Adds an array, shared aggregate, or callable-result edge.
///
/// Complexity: O(1) time and new metadata space.
cel::value_type extend_metadata(const cel::value_type& child, std::size_t level) {
    switch (level % 4) {
        case 0:
            return cel::value_type::array(child);
        case 1:
            return cel::value_type::tuple(std::array{child, child});
        case 2:
            return cel::value_type::record("shared", std::array{cel::record_field{"left", child},
                                                                cel::record_field{"right", child}});
        default:
            return cel::value_type::callable({}, child);
    }
}

/// Catches invalidating retained metadata during concurrent root and interior-alias release.
TEST_CASE("concurrent metadata release preserves retained shared descendants") {
    constexpr std::size_t depth = 12000;
    auto type = cel::value_type::leaf<bool>();
    auto expected = cel::value_type::leaf<bool>();
    std::vector<cel::value_type> aliases;
    // Construct a second, independent shape for the descendant that survives retirement.
    for (std::size_t i = 0; i != depth / 2; ++i)
        expected = extend_metadata(expected, i);
    for (std::size_t i = 0; i != depth; ++i) {
        type = extend_metadata(type, i);
        aliases.push_back(type);
    }
    const auto survivor = aliases[depth / 2 - 1];
    std::vector<cel::value_type> roots{type};
    type = cel::value_type{};
    run_concurrently([owners = std::move(roots)]() mutable { owners.clear(); },
                     [owners = std::move(aliases)]() mutable {
                         while (!owners.empty())
                             owners.pop_back();
                     });
    CHECK(survivor.same_shape(expected));
    CHECK(survivor.result().kind() == cel::type_kind::record);
}
/// Supplies a nonowning client field with extended alignment.
struct alignas(256) ownership_aligned_leaf {
    std::array<std::byte, 256> bytes; ///< Stores a trivially copyable opaque payload.
};

/// Catches incorrect range offsets when zero-byte and over-aligned fields precede carriers.
TEST_CASE("record ownership ranges preserve canonical offsets around nonowning fields") {
    const auto array = cel::value::from(std::vector<double>{5.0});
    const auto wide = cel::value::from(ownership_aligned_leaf{});
    const auto type =
        cel::value_type::record("mixed", std::array{cel::record_field{"unit", cel::value_type{}},
                                                    cel::record_field{"bytes", wide.type()},
                                                    cel::record_field{"items", array.type()}});
    auto original = cel::value::record(type, std::array{cel::value{}, wide, array});
    auto copy = original;
    copy.set_field("items", cel::value::from(std::vector<double>{9.0}));
    CHECK(original.field("items").at(0).as<double>() == 5.0);
    CHECK(copy.field("items").at(0).as<double>() == 9.0);
    original = cel::value{};
    copy = cel::value{};
}
/// Catches recursive last-reference release, including during exception cleanup.
TEST_CASE("deep descriptor release and exception unwinding are stack independent") {
    const cel::value_type unit;
    {
        auto type = cel::value_type::leaf<bool>();
        for (std::size_t i = 0; i != 50000; ++i)
            type = cel::value_type::array(type);
        type = unit;
        CHECK(type.same_shape(unit));
    }
    CHECK_THROWS_AS(
        [&] {
            auto type = cel::value_type::leaf<bool>();
            for (std::size_t i = 0; i != 50000; ++i)
                type = cel::value_type::array(type);
            throw std::runtime_error("unwind deep metadata");
        }(),
        std::runtime_error);
}

/// Catches exponential admission of compact shared zero-byte tuple metadata.
TEST_CASE("compact tuple DAG parameters do not expand their shared subgraphs") {
    cel::value_type type;
    cel::value_type equal;
    for (std::size_t i = 0; i != 27; ++i) {
        type = cel::value_type::tuple(std::array{type, type});
        equal = cel::value_type::tuple(std::array{equal, equal});
    }
    const auto callable =
        cel::value_type::callable(std::array{type}, cel::value_type::leaf<bool>());
    CHECK(callable.parameters()[0].same_shape(equal));
    CHECK(type.byte_layout().size == 0);
}

/// Catches recursive comparison or callable validation of deeply admitted metadata.
TEST_CASE("deep metadata comparison and parameter admission are stack independent") {
    auto a = cel::value_type::leaf<bool>();
    auto b = cel::value_type::leaf<bool>();
    for (std::size_t i = 0; i != 50000; ++i) {
        a = cel::value_type::array(a);
        b = cel::value_type::array(b);
    }
    CHECK(a.same_shape(b));
    CHECK(
        cel::value_type::callable(std::array{a}, cel::value_type{}).parameters()[0].same_shape(b));
}

/// Verifies scalar array conversion and independent copies at different sizes.
TEST_CASE("scalar canonical arrays preserve values and independent copies") {
    const std::vector<double> small_input(16, 2.0);
    const std::vector<double> large_input(4096, 3.0);
    auto small = cel::value::from(small_input);
    auto large = cel::value::from(large_input);
    CHECK(small.as<std::vector<double>>() == small_input);
    auto clone = large;
    clone.set(0, cel::value::from(7.0));
    CHECK(large.at(0).as<double>() == 3.0);
    CHECK(clone.at(4095).as<double>() == 3.0);
    const auto scalar = cel::value::from(2.0);
    const std::vector<cel::value> small_children(16, scalar);
    const std::vector<cel::value> large_children(4096, scalar);
    small = cel::value::array(scalar.type(), small_children);
    large = cel::value::array(scalar.type(), large_children);
    CHECK(small.size() == small_children.size());
    CHECK(large.size() == large_children.size());
    CHECK(large.at(4095).as<double>() == 2.0);
}

/// Verifies canonical tuple array conversion at different sizes.
TEST_CASE("nonowning tuple array elements preserve their values") {
    const std::vector<std::tuple<bool, double>> small_input(16, {true, 2.0});
    const std::vector<std::tuple<bool, double>> large_input(4096, {false, 3.0});
    const auto small = cel::value::from(small_input);
    const auto large = cel::value::from(large_input);
    CHECK(small.as<std::vector<std::tuple<bool, double>>>() == small_input);
    CHECK(large.at(4095).as<std::tuple<bool, double>>() == std::tuple{false, 3.0});
}
