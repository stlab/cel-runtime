// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_LAYOUT_HPP
#define CEL_RUNTIME_LAYOUT_HPP

#include <cstddef>

/// Provides checked layouts for aligned runtime byte storage.
/// Example: `cel::align_offset(1, 64) == 64`.
namespace cel {

/// Describes a byte extent and its requested alignment.
/// Valid alignment is a nonzero power of two; operations validate it at their boundaries.
/// Example: `cel::layout leaf{sizeof(double), alignof(double)};`.
struct layout {
    /// Counts bytes in the extent.
    std::size_t size;
    /// Specifies the requested alignment in bytes.
    std::size_t alignment;
};

/// Returns the smallest multiple of alignment not less than offset.
/// Throws std::invalid_argument for zero or non-power-of-two alignment.
/// Throws std::overflow_error if the rounded offset cannot fit in std::size_t.
/// Example: `cel::align_offset(1, 64) == 64`.
std::size_t align_offset(std::size_t offset, std::size_t alignment);

} // namespace cel
#endif
