// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_STORAGE_HPP
#define CEL_RUNTIME_STORAGE_HPP

#include <cstddef>
#include <memory>

namespace cel::detail {

/// Returns the smallest multiple of alignment not less than offset.
///
/// Throws std::invalid_argument unless alignment is a nonzero power of two,
/// and std::overflow_error if rounding cannot fit in std::size_t.
std::size_t align_offset(std::size_t offset, std::size_t alignment);

/// Returns the sum of two byte extents or counts.
///
/// Throws std::overflow_error when the sum cannot fit in std::size_t.
std::size_t checked_add(std::size_t left, std::size_t right);

/// A deleter for an allocation obtained from aligned operator new.
///
/// The recorded alignment matches the allocation being released.
struct aligned_delete {
    /// Records the alignment of the owned allocation.
    std::size_t alignment = alignof(std::max_align_t);

    /// Releases data; null owns no allocation.
    ///
    /// \pre Nonnull data is the unreleased allocation returned by aligned operator new
    /// with this alignment, which is a nonzero power of two.
    void operator()(std::byte* data) const noexcept;
};

/// A unique owner of aligned bytes with an initialized prefix.
///
/// size() <= capacity(); alignment() never decreases except on move.
/// Default-constructed and moved-from objects have null data, zero size, and zero capacity.
/// Zero size may retain an allocation; reserve never reduces capacity.
/// Owns bytes, not object destructors; only bytewise-transferable payloads belong here.
/// Borrowed pointers expire on reallocation, destruction, or ownership transfer.
///
/// Example: `storage bytes; bytes.reserve(256, 256); bytes.resize(256);`.
class storage {
    // [knowledge] _size <= _capacity; the deleter alignment describes _data even when
    // there is no allocation. Spare capacity is not part of the initialized prefix.
    /// Owns the aligned allocation, if any.
    std::unique_ptr<std::byte, aligned_delete> _data;
    /// Records the initialized prefix byte count.
    std::size_t _size = 0;
    /// Records the reserved byte count.
    std::size_t _capacity = 0;

public:
    /// Creates empty storage without allocating.
    storage() noexcept = default;
    /// Releases the owned allocation without destroying payload objects.
    ~storage() = default;
    /// Disallows shared or copied ownership.
    storage(const storage&) = delete;
    /// Disallows copy assignment.
    storage& operator=(const storage&) = delete;
    /// Transfers ownership and leaves other empty with default alignment.
    storage(storage&& other) noexcept;
    /// Releases prior ownership and transfers other, leaving other empty with default alignment.
    ///
    /// Self-move leaves ownership unchanged.
    storage& operator=(storage&& other) noexcept;

    /// Ensures capacity >= bytes and alignment >= both old and requested alignment.
    ///
    /// Preserves size and initialized bytes; zero bytes need not allocate.
    ///
    /// Validates alignment as a nonzero power of two, throwing std::invalid_argument.
    /// Throws std::overflow_error for capacity arithmetic overflow and std::bad_alloc
    /// for allocation failure. Errors leave the storage unchanged.
    ///
    /// Complexity: O(initialized byte count) transfer time on growth or realignment,
    /// O(resulting capacity) owned storage, and O(1) auxiliary space.
    void reserve(std::size_t bytes, std::size_t alignment);

    /// Sets the initialized extent, zero-initializing newly exposed bytes.
    ///
    /// Preserves the retained prefix and alignment; shrinking retains capacity.
    ///
    /// Propagates reserve errors without changing the storage.
    ///
    /// Complexity: O(newly exposed bytes) time plus O(prior initialized bytes) transfer
    /// time if reallocation is needed; O(resulting capacity) owned storage and O(1)
    /// auxiliary space. Shrinking takes constant time.
    void resize(std::size_t bytes);

    /// Borrows the allocation; only the initialized prefix may be read.
    ///
    /// The pointer aliases owned storage and expires on reallocation, destruction, or
    /// ownership transfer. Shrinking revokes read access to the removed suffix.
    std::byte* data() noexcept { return _data.get(); }
    /// Borrows the allocation for reading the initialized prefix.
    ///
    /// The pointer aliases owned storage and expires on reallocation, destruction, or
    /// ownership transfer. Shrinking revokes read access to the removed suffix.
    const std::byte* data() const noexcept { return _data.get(); }
    /// Returns the initialized byte count.
    std::size_t size() const noexcept { return _size; }
    /// Returns the allocated byte count.
    std::size_t capacity() const noexcept { return _capacity; }
    /// Returns the allocation alignment, at least alignof(std::max_align_t).
    std::size_t alignment() const noexcept { return _data.get_deleter().alignment; }
};

} // namespace cel::detail
#endif
