// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_LAYOUT_HPP
#define CEL_RUNTIME_LAYOUT_HPP

#include <cstddef>

/// Provides byte extents for stack reservation and type descriptors.
namespace cel {
/// \addtogroup runtime_values
/// @{

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

/// @}
} // namespace cel
#endif
