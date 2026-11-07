// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CANONICAL_HPP
#define CEL_RUNTIME_CANONICAL_HPP
#include "storage.hpp"
#include "value_ops.hpp"
#include <cstring>

namespace cel::detail {
/// Returns an offset within canonical storage, retaining null for zero-byte extents.
/// Precondition: offset lies within the allocation, or is zero when data is null.
inline std::byte* canonical_at(std::byte* data, std::size_t offset) noexcept {
    return data ? data + offset : nullptr;
}
/// Copies canonical bytes without duplicating ownership; the caller retires the source.
/// Precondition: disjoint extents contain size readable/writable bytes.
/// Complexity: O(size).
inline void transfer_bytes(std::byte* destination,
                           const std::byte* source,
                           std::size_t size) noexcept {
    if (size != 0) std::memcpy(destination, source, size);
}
/// Guards one canonical owner or a transferred suffix of a fixed tuple.
/// Allocates outer storage before transfers; cleanup uses existing ownership operations.
struct canonical_owner {
    value_type type;        ///< Retains complete destruction metadata.
    storage bytes;          ///< Owns stable canonical storage.
    bool live = false;      ///< Identifies a completely initialized owner.
    std::size_t suffix = 0; ///< Counts transferred trailing tuple fields.
    /// Allocates aligned canonical storage before any child ownership transfers.
    /// Complexity: O(canonical extent) time and space.
    explicit canonical_owner(value_type shape) : type(std::move(shape)) {
        const auto extent = type.byte_layout();
        bytes.reserve(extent.size, extent.alignment);
        bytes.resize(extent.size);
    }
    /// Retires complete ownership or the initialized tuple suffix without allocation.
    /// Complexity: O(visited ownership paths and suffix fields).
    ~canonical_owner() {
        if (live)
            destroy_payload(type, bytes.data());
        else if (suffix != 0) {
            const auto fields = type.fields();
            for (std::size_t i = fields.size() - suffix; i != fields.size(); ++i)
                destroy_payload(fields[i], canonical_at(bytes.data(), type.offsets()[i]));
        }
    }
    /// Disallows duplicate canonical ownership.
    canonical_owner(const canonical_owner&) = delete;
    /// Disallows duplicate assignment.
    canonical_owner& operator=(const canonical_owner&) = delete;
};
/// Transfers a guarded complete owner into a prepared stack destination.
/// Precondition: source owns exactly one initialized owner with matching destination type.
/// Complexity: O(outer canonical bytes); no allocation.
inline void write_owner(const void* source, const value_type& type, std::byte* destination) {
    auto& owner = *static_cast<canonical_owner*>(const_cast<void*>(source));
    transfer_bytes(destination, owner.bytes.data(), type.byte_layout().size);
    owner.live = false;
}
} // namespace cel::detail
#endif
