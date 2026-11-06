// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_VALUE_OPS_HPP
#define CEL_RUNTIME_VALUE_OPS_HPP

#include <cel/runtime/value.hpp>

namespace cel::detail {

/// Clones one initialized raw value into uninitialized canonical storage.
/// Precondition: destination is an uninitialized, suitably aligned extent of type.
/// When opaque operations exist, source is a disjoint initialized canonical extent.
/// A deferred callable is rejected before source bytes are read.
/// Throws std::invalid_argument if callable ownership has not been installed.
/// Failure destroys every constructed child; destination remains uninitialized.
/// Complexity: O(canonical bytes + visited ownership paths) time and new owned space.
/// Traversal uses O(1) auxiliary space and worklist links in newly owned array nodes.
void clone_payload(const value_type& type, const std::byte* source, std::byte* destination);

/// Destroys an initialized raw value, retiring ownership without releasing its outer bytes.
/// Precondition: data contains exactly one valid owned representation of type.
/// Never allocates or throws. Bytewise transfer must retire the source without this call.
/// Complexity: O(visited ownership paths and array nodes) time and O(1) auxiliary space.
/// Scalar arrays need no per-element cleanup; nested arrays supply their own worklist links.
void destroy_payload(const value_type& type, std::byte* data) noexcept;

/// Bridges trusted owning wrappers to canonical raw representations.
/// Does not permit byte-relocation of the public wrapper itself.
struct value_access {
    /// Borrows initialized canonical bytes until the owner changes or dies.
    static const std::byte* data(const value& owner) noexcept;
    /// Copies initialized canonical bytes into a new independent public owner.
    /// Precondition: data is a valid owned representation of type.
    /// Complexity: O(payload size) time and new owned space.
    static value copy(const value_type& type, const std::byte* data);
    /// Takes uniquely owned boundary elements into packed canonical storage.
    /// Sources become source unit as each element transfers ownership.
    /// Throws std::invalid_argument for shape/operation mismatch, destroying completed transfers.
    /// Remaining sources retain ownership.
    /// Throws std::overflow_error for canonical extent overflow before consuming sources.
    /// Complexity: O(canonical element bytes + compared shape size) time and packed outer storage.
    static value take_array(value_type element, std::span<value> children);
};

} // namespace cel::detail
#endif
