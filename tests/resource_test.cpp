// SPDX-License-Identifier: BSL-1.0
#include "value_ops.hpp"
#include <cel/runtime/value.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <limits>
#include <stdexcept>
#include <vector>
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

/// Catches recursive cloning and destruction of owned array trees.
TEST_CASE("deep owned array trees clone release and unwind without recursive cleanup") {
    auto source = cel::value::from(true);
    for (std::size_t i = 0; i != 50000; ++i)
        source = cel::detail::value_access::take_array(source.type(), std::span(&source, 1));
    auto copy = source;
    CHECK(copy.type().same_shape(source.type()));
    CHECK(copy.size() == 1);
    copy = cel::value{};
    source = cel::value{};
    CHECK_THROWS_AS(
        [] {
            auto tree = cel::value::from(false);
            for (std::size_t i = 0; i != 50000; ++i)
                tree = cel::detail::value_access::take_array(tree.type(), std::span(&tree, 1));
            throw std::runtime_error("unwind packed ownership");
        }(),
        std::runtime_error);
}

/// Catches recursion through deeply nested inline fields even when only one carrier exists.
TEST_CASE("deep inline aggregate ownership paths are stack independent") {
    const auto leaf_array = cel::value::from(std::vector<double>{2.0, 3.0});
    auto type = leaf_array.type();
    for (std::size_t i = 0; i != 50000; ++i)
        type = cel::value_type::tuple(std::array{type});
    auto owner = cel::detail::value_access::copy(type, cel::detail::value_access::data(leaf_array));
    auto copy = owner;
    CHECK(copy.type().same_shape(type));
    owner = cel::value{};
    copy = cel::value{};
}
