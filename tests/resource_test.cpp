// SPDX-License-Identifier: BSL-1.0
#include "value_ops.hpp"
#include <cel/runtime/value.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <array>
#include <cstdlib>
#include <limits>
#include <new>
#include <stdexcept>
#include <vector>
#if defined(_WIN32)
#include <malloc.h>
#endif

/// Counts ordinary and aligned allocation calls inside single-threaded measurement scopes.
std::size_t allocation_calls = 0;
/// Enables allocation measurement without allocating measurement state.
bool count_allocations = false;

/// Replaces ordinary allocation to detect per-element boxes.
/// Throws std::bad_alloc if allocation fails.
void* operator new(std::size_t size) {
    if (count_allocations) ++allocation_calls;
    if (void* result = std::malloc(size == 0 ? 1 : size)) return result;
    throw std::bad_alloc();
}

/// Releases an ordinary allocation.
void operator delete(void* pointer) noexcept { std::free(pointer); }

/// Replaces aligned allocation to count canonical byte allocations as well as boxes.
/// Throws std::bad_alloc on extent overflow or allocation failure.
void* operator new(std::size_t size, std::align_val_t alignment) {
    if (count_allocations) ++allocation_calls;
    const auto align = static_cast<std::size_t>(alignment);
    const auto extent = size == 0 ? std::size_t{1} : size;
    if (extent > std::numeric_limits<std::size_t>::max() - (align - 1)) throw std::bad_alloc();
    const auto rounded = ((extent + align - 1) / align) * align;
#if defined(_WIN32)
    void* result = _aligned_malloc(rounded, align);
#else
    void* result = std::aligned_alloc(align, rounded);
#endif
    if (!result) throw std::bad_alloc();
    return result;
}

/// Releases an instrumented aligned allocation with the matching platform facility.
void operator delete(void* pointer, std::align_val_t) noexcept {
#if defined(_WIN32)
    _aligned_free(pointer);
#else
    std::free(pointer);
#endif
}

/// Releases an ordinary allocation when the compiler supplies its extent.
void operator delete(void* pointer, std::size_t) noexcept { ::operator delete(pointer); }
/// Releases an aligned allocation when the compiler supplies its extent.
void operator delete(void* pointer, std::size_t, std::align_val_t alignment) noexcept {
    ::operator delete(pointer, alignment);
}

/// Measures allocations without counting assertions or input fixtures.
struct allocation_scope {
    /// Begins single-threaded allocation measurement.
    allocation_scope() {
        allocation_calls = 0;
        count_allocations = true;
    }
    /// Ends measurement without allocating.
    ~allocation_scope() { count_allocations = false; }
};

/// Catches recursive last-reference release and allocations hidden in cleanup.
TEST_CASE("deep descriptor release and exception unwinding are stack independent") {
    const cel::value_type unit;
    {
        auto type = cel::value_type::leaf<bool>();
        for (std::size_t i = 0; i != 50000; ++i)
            type = cel::value_type::array(type);
        {
            allocation_scope scope;
            type = unit;
        }
        CHECK(allocation_calls == 0);
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

/// Catches scalar array construction or cloning allocating one public owner per element.
TEST_CASE("scalar canonical arrays allocate per array rather than per element") {
    const std::vector<double> small_input(16, 2.0);
    const std::vector<double> large_input(4096, 3.0);
    (void)cel::value::from(small_input);
    cel::value small;
    cel::value large;
    std::size_t small_calls = 0;
    std::size_t large_calls = 0;
    {
        allocation_scope scope;
        small = cel::value::from(small_input);
        small_calls = allocation_calls;
    }
    {
        allocation_scope scope;
        large = cel::value::from(large_input);
        large_calls = allocation_calls;
    }
    CHECK(large_calls <= small_calls + 16);
    cel::value clone;
    {
        allocation_scope scope;
        clone = small;
        small_calls = allocation_calls;
    }
    {
        allocation_scope scope;
        clone = large;
        large_calls = allocation_calls;
    }
    CHECK(large_calls <= small_calls + 16);
    clone.set(0, cel::value::from(7.0));
    CHECK(large.at(0).as<double>() == 3.0);
    CHECK(clone.at(4095).as<double>() == 3.0);
    const auto scalar = cel::value::from(2.0);
    const std::vector<cel::value> small_children(16, scalar);
    const std::vector<cel::value> large_children(4096, scalar);
    {
        allocation_scope scope;
        small = cel::value::array(scalar.type(), small_children);
        small_calls = allocation_calls;
    }
    {
        allocation_scope scope;
        large = cel::value::array(scalar.type(), large_children);
        large_calls = allocation_calls;
    }
    CHECK(large_calls <= small_calls + 16);
}

/// Catches scalar field boxes inside canonical tuple elements.
TEST_CASE("nonowning tuple array elements also avoid per element allocation") {
    const std::vector<std::tuple<bool, double>> small_input(16, {true, 2.0});
    const std::vector<std::tuple<bool, double>> large_input(4096, {false, 3.0});
    (void)cel::value::from(small_input);
    cel::value small;
    cel::value large;
    std::size_t small_calls = 0;
    std::size_t large_calls = 0;
    {
        allocation_scope scope;
        small = cel::value::from(small_input);
        small_calls = allocation_calls;
    }
    {
        allocation_scope scope;
        large = cel::value::from(large_input);
        large_calls = allocation_calls;
    }
    CHECK(large_calls <= small_calls + 16);
    CHECK(large.at(4095).as<std::tuple<bool, double>>() == std::tuple{false, 3.0});
}

/// Catches recursive cloning/destruction of owned array trees or allocation during cleanup.
TEST_CASE("deep owned array trees clone release and unwind without recursive cleanup") {
    auto source = cel::value::from(true);
    for (std::size_t i = 0; i != 50000; ++i)
        source = cel::detail::value_access::take_array(source.type(), std::span(&source, 1));
    auto copy = source;
    CHECK(copy.type().same_shape(source.type()));
    CHECK(copy.size() == 1);
    {
        allocation_scope scope;
        copy = cel::value{};
        source = cel::value{};
    }
    CHECK(allocation_calls == 0);
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
    {
        allocation_scope scope;
        owner = cel::value{};
        copy = cel::value{};
    }
    CHECK(allocation_calls == 0);
}
