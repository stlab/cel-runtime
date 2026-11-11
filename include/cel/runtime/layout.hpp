// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_LAYOUT_HPP
#define CEL_RUNTIME_LAYOUT_HPP

#include <cstddef>

/// Provides checked layouts for aligned runtime byte storage.
/// Example: `cel::align_offset(1, 64) == 64`.
namespace cel {

/// Describes a byte extent and its requested alignment, both in bytes.
/// A checked storage operation accepts a zero size but requires a nonzero power-of-two
/// alignment. The fields describe storage, not initialized objects or their ownership.
/// Example: `cel::layout leaf{sizeof(double), alignof(double)};`.
struct layout {
    /// Counts bytes in the extent; zero describes an empty extent.
    std::size_t size;
    /// Specifies the requested nonzero power-of-two alignment in bytes.
    std::size_t alignment;
};

/// Returns the smallest multiple of alignment not less than offset, in bytes.
/// Throws std::invalid_argument for zero or non-power-of-two alignment.
/// Throws std::overflow_error if the rounded offset cannot fit in std::size_t.
/// Example: `cel::align_offset(1, 64) == 64`.
std::size_t align_offset(std::size_t offset, std::size_t alignment);

} // namespace cel
#endif
