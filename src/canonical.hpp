// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_CANONICAL_HPP
#define CEL_RUNTIME_CANONICAL_HPP
#include "storage.hpp"
#include "value_ops.hpp"
#include <cstring>

namespace cel::detail {
/// Returns an offset within canonical storage, retaining null for zero-byte extents.
///
/// \pre offset lies within the allocation, or is zero when data is null.
/// The result borrows the same allocation and has its lifetime.
inline std::byte* canonical_at(std::byte* data, std::size_t offset) noexcept {
    return data ? data + offset : nullptr;
}
/// Copies canonical bytes without duplicating ownership; the caller retires the source.
///
/// \pre Disjoint extents contain size readable/writable bytes; source has valid canonical
/// bytes, and destination is uninitialized ownership storage.
/// Retiring the transferred source must not destroy its payload.
/// Complexity: O(size) time and O(1) auxiliary space.
inline void transfer_bytes(std::byte* destination,
                           const std::byte* source,
                           std::size_t size) noexcept {
    if (size != 0) std::memcpy(destination, source, size);
}
/// Exclusive ownership of a complete canonical value or an initialized tuple suffix.
///
/// Cleanup retires exactly the initialized ownership without runtime traversal allocation.
struct canonical_owner {
    // [knowledge] live means all bytes contain a complete owner. Otherwise suffix counts
    // exactly the initialized trailing tuple fields; type and bytes retain their layout.
    value_type type;        // Retains complete destruction metadata.
    storage bytes;          // Owns stable canonical storage.
    bool live = false;      // Identifies a completely initialized owner.
    std::size_t suffix = 0; // Counts transferred trailing tuple fields.
    /// Allocates aligned canonical storage before any child ownership transfers.
    ///
    /// Begins with no initialized ownership.
    /// Complexity: O(canonical extent) time and result storage.
    explicit canonical_owner(value_type shape) : type(std::move(shape)) {
        const auto extent = type.byte_layout();
        bytes.reserve(extent.size, extent.alignment);
        bytes.resize(extent.size);
    }
    /// Retires complete ownership or the initialized tuple suffix without allocation.
    ///
    /// Runtime retirement uses no nesting-dependent call space; client operations retain
    /// their costs and behavior.
    /// Complexity: O(F + N + L + C + D) time and O(1) runtime auxiliary space, with
    /// costs as in value and initialized suffix fields included in F.
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
///
/// \pre source points to an exclusively accessible canonical_owner with live complete
/// ownership and matching type; destination is disjoint aligned uninitialized storage
/// of that canonical extent and retains complete destruction metadata.
/// The guard no longer destroys transferred ownership; its outer bytes remain guard-owned.
/// Complexity: O(outer canonical bytes) time and O(1) auxiliary space; no allocation.
inline void write_owner(const void* source, const value_type& type, std::byte* destination) {
    auto& owner = *static_cast<canonical_owner*>(const_cast<void*>(source));
    transfer_bytes(destination, owner.bytes.data(), type.byte_layout().size);
    owner.live = false;
}
} // namespace cel::detail
#endif
