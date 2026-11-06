// SPDX-License-Identifier: BSL-1.0
#include "storage.hpp"
#include <cel/runtime/layout.hpp>

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <type_traits>
#include <utility>

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

/// Catches incorrect rounding of zero, already-aligned, and unaligned offsets.
TEST_CASE("align_offset rounds upward without changing aligned offsets") {
    CHECK(cel::align_offset(0, 64) == 0);
    CHECK(cel::align_offset(1, 64) == 64);
    CHECK(cel::align_offset(64, 64) == 64);
    CHECK(cel::align_offset(65, 64) == 128);
    CHECK(cel::align_offset(7, 1) == 7);
    CHECK(cel::align_offset(std::numeric_limits<std::size_t>::max(), 1) ==
          std::numeric_limits<std::size_t>::max());
}

/// Catches unchecked alignment and offset overflow at the safe boundary.
TEST_CASE("align_offset rejects invalid alignment and arithmetic overflow") {
    CHECK_THROWS_AS(cel::align_offset(1, 0), std::invalid_argument);
    CHECK_THROWS_AS(cel::align_offset(1, 3), std::invalid_argument);
    CHECK_THROWS_AS(cel::align_offset(std::numeric_limits<std::size_t>::max(), 64),
                    std::overflow_error);
    constexpr auto highest = std::numeric_limits<std::size_t>::max() / 2 + 1;
    CHECK(cel::align_offset(highest, highest) == highest);
    CHECK_THROWS_AS(cel::align_offset(highest + 1, highest), std::overflow_error);
}

/// Catches accidental allocation for zero bytes or forgotten empty-state alignment.
TEST_CASE("empty storage remains allocation-free and remembers alignment") {
    cel::detail::storage bytes;
    CHECK(bytes.data() == nullptr);
    CHECK(bytes.size() == 0);
    CHECK(bytes.capacity() == 0);
    bytes.reserve(0, 256);
    CHECK(bytes.data() == nullptr);
    CHECK(bytes.capacity() == 0);
    CHECK(bytes.alignment() >= 256);
    bytes.resize(1);
    CHECK(reinterpret_cast<std::uintptr_t>(bytes.data()) % 256 == 0);
    CHECK(bytes.data()[0] == std::byte{0});
}

/// Catches loss of initialized bytes, reserve changing size, or narrowing alignment.
TEST_CASE("reserve preserves initialized bytes through growth and realignment") {
    cel::detail::storage bytes;
    bytes.resize(4);
    const std::array payload{std::byte{0x11}, std::byte{0x22}, std::byte{0x33}, std::byte{0x44}};
    std::memcpy(bytes.data(), payload.data(), payload.size());
    const auto growth_request = bytes.capacity() + 1;
    bytes.reserve(growth_request, 64);
    CHECK(bytes.capacity() >= growth_request);
    CHECK(bytes.size() == 4);
    CHECK(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
    const auto capacity = bytes.capacity();
    bytes.reserve(4, 256);
    CHECK(bytes.capacity() >= capacity);
    CHECK(bytes.alignment() >= 256);
    CHECK(reinterpret_cast<std::uintptr_t>(bytes.data()) % 256 == 0);
    CHECK(std::memcmp(bytes.data(), payload.data(), payload.size()) == 0);
    auto* const address = bytes.data();
    bytes.reserve(1, 1);
    CHECK(bytes.data() == address);
    CHECK(bytes.alignment() >= 256);
    CHECK(bytes.size() == 4);
    const auto& view = bytes;
    CHECK(view.data()[3] == std::byte{0x44});
}

/// Catches uninitialized exposed bytes or shrinking that loses the retained prefix.
TEST_CASE("resize initializes growth and retains capacity on shrink") {
    cel::detail::storage bytes;
    bytes.resize(2);
    bytes.data()[0] = std::byte{0x55};
    bytes.data()[1] = std::byte{0x66};
    const auto growth_request = bytes.capacity() + 3;
    bytes.resize(growth_request);
    CHECK(bytes.size() == growth_request);
    CHECK(bytes.capacity() >= growth_request);
    CHECK(bytes.data()[0] == std::byte{0x55});
    CHECK(bytes.data()[1] == std::byte{0x66});
    CHECK(std::all_of(bytes.data() + 2, bytes.data() + growth_request,
                      [](std::byte value) { return value == std::byte{0}; }));
    const auto capacity = bytes.capacity();
    bytes.resize(1);
    CHECK(bytes.capacity() == capacity);
    bytes.resize(2);
    CHECK(bytes.data()[0] == std::byte{0x55});
    CHECK(bytes.data()[1] == std::byte{0});
    bytes.resize(0);
    CHECK(bytes.size() == 0);
    CHECK(bytes.capacity() == capacity);
}

/// Catches silent arithmetic failure or mutation before an unsuccessful reserve.
TEST_CASE("storage reports invalid alignment and allocation arithmetic overflow") {
    cel::detail::storage bytes;
    bytes.resize(1);
    bytes.data()[0] = std::byte{0x77};
    auto* const address = bytes.data();
    const auto capacity = bytes.capacity();
    const auto alignment = bytes.alignment();
    CHECK_THROWS_AS(bytes.reserve(1, 0), std::invalid_argument);
    CHECK_THROWS_AS(bytes.reserve(1, 3), std::invalid_argument);
    CHECK_THROWS_AS(bytes.reserve(std::numeric_limits<std::size_t>::max(), 64),
                    std::overflow_error);
    CHECK_THROWS_AS(bytes.resize(std::numeric_limits<std::size_t>::max()), std::overflow_error);
    CHECK(bytes.data() == address);
    CHECK(bytes.capacity() == capacity);
    CHECK(bytes.alignment() == alignment);
    CHECK(bytes.size() == 1);
    CHECK(bytes.data()[0] == std::byte{0x77});
}

/// Catches allocation failures converted into success or loss of prior ownership.
TEST_CASE("storage propagates allocation failure without changing initialized bytes") {
    cel::detail::storage bytes;
    bytes.resize(1);
    bytes.data()[0] = std::byte{0x99};
    auto* const address = bytes.data();
    const auto capacity = bytes.capacity();
    const auto alignment = bytes.alignment();
    // This representable, aligned extent exceeds the available address space.
    constexpr auto impossible = std::numeric_limits<std::size_t>::max() - 255;
    CHECK_THROWS_AS(bytes.reserve(impossible, 256), std::bad_alloc);
    CHECK(bytes.data() == address);
    CHECK(bytes.size() == 1);
    CHECK(bytes.capacity() == capacity);
    CHECK(bytes.alignment() == alignment);
    CHECK(bytes.data()[0] == std::byte{0x99});
}

/// Catches misaligned allocation or byte transfer that corrupts implicit-lifetime payloads.
TEST_CASE("storage holds and transfers an alignas 256 trivially copyable leaf") {
    static_assert(std::is_trivially_copyable_v<leaf>);
    cel::detail::storage bytes;
    bytes.reserve(sizeof(leaf), alignof(leaf));
    CHECK(bytes.capacity() >= sizeof(leaf));
    bytes.resize(sizeof(leaf));
    const leaf original{0x123456789abcdef0};
    std::memcpy(bytes.data(), &original, sizeof(original));
    CHECK(reinterpret_cast<std::uintptr_t>(bytes.data()) % 256 == 0);
    CHECK(reinterpret_cast<const leaf*>(bytes.data())->value == original.value);
    const auto growth_request = bytes.capacity() + 1;
    bytes.reserve(growth_request, 1);
    CHECK(bytes.capacity() >= growth_request);
    CHECK(reinterpret_cast<std::uintptr_t>(bytes.data()) % 256 == 0);
    CHECK(reinterpret_cast<const leaf*>(bytes.data())->value == original.value);
    leaf restored{};
    std::memcpy(&restored, bytes.data(), sizeof(restored));
    CHECK(restored.value == original.value);
}

// Moved-from observations verify storage's documented empty and reusable state.
// NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
/// Catches duplicated ownership, stale moved-from metadata, or lost destination cleanup.
TEST_CASE("storage moves unique ownership and leaves its source reusable") {
    static_assert(!std::is_copy_constructible_v<cel::detail::storage>);
    static_assert(!std::is_copy_assignable_v<cel::detail::storage>);
    static_assert(std::is_nothrow_move_constructible_v<cel::detail::storage>);
    static_assert(std::is_nothrow_move_assignable_v<cel::detail::storage>);
    cel::detail::storage source;
    source.reserve(4, 256);
    source.resize(4);
    source.data()[0] = std::byte{0x88};
    auto* const address = source.data();
    cel::detail::storage moved(std::move(source));
    CHECK(moved.data() == address);
    CHECK(moved.size() == 4);
    CHECK(moved.alignment() >= 256);
    CHECK(source.data() == nullptr);
    CHECK(source.size() == 0);
    CHECK(source.capacity() == 0);
    source.resize(1);
    CHECK(source.data()[0] == std::byte{0});
    source = std::move(moved);
    CHECK(source.data() == address);
    CHECK(source.data()[0] == std::byte{0x88});
    CHECK(moved.data() == nullptr);
    CHECK(moved.size() == 0);
    CHECK(moved.capacity() == 0);
    auto& alias = source;
    source = std::move(alias);
    CHECK(source.data() == address);
    cel::detail::storage empty;
    source = std::move(empty);
    CHECK(source.data() == nullptr);
    CHECK(source.size() == 0);
    CHECK(source.capacity() == 0);
}
// NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)

// Moved-from alignment observations verify the documented reset.
// NOLINTBEGIN(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
/// Catches alignment metadata lost while moving allocation-free storage.
TEST_CASE("storage moves alignment even when no allocation exists") {
    cel::detail::storage source;
    source.reserve(0, 256);
    cel::detail::storage destination(std::move(source));
    CHECK(destination.data() == nullptr);
    CHECK(destination.size() == 0);
    CHECK(destination.capacity() == 0);
    CHECK(destination.alignment() >= 256);
    CHECK(source.alignment() == alignof(std::max_align_t));
    source = std::move(destination);
    CHECK(source.alignment() >= 256);
    CHECK(destination.alignment() == alignof(std::max_align_t));
    source.resize(1);
    CHECK(reinterpret_cast<std::uintptr_t>(source.data()) % 256 == 0);
}
// NOLINTEND(bugprone-use-after-move,clang-analyzer-cplusplus.Move)
