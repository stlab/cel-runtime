// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/layout.hpp>
#include <cstdint>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
/// Supplies a trivially copyable, over-aligned payload.
struct alignas(256) leaf {
    /// Stores a scalar whose representation survives byte transfer.
    std::uint64_t value;
};

/// Verifies byte extent and alignment describe a usable over-aligned payload.
TEST_CASE("layout describes an over-aligned leaf") {
    const cel::layout shape{sizeof(leaf), alignof(leaf)};
    CHECK(shape.size == 256);
    CHECK(shape.alignment == 256);
}
