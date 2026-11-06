// SPDX-License-Identifier: BSL-1.0
#ifndef CEL_RUNTIME_STORAGE_HPP
#define CEL_RUNTIME_STORAGE_HPP

#include <cstddef>
#include <memory>

namespace cel::detail {

/// Releases an aligned allocation using the alignment supplied to operator new.
/// Invariant: alignment is a nonzero power of two matching the owned allocation.
struct aligned_delete {
    /// Records the alignment of the owned allocation.
    std::size_t alignment = alignof(std::max_align_t);

    /// Releases data; null owns no allocation.
    /// Precondition: nonnull data owns an unreleased allocation with this alignment.
    void operator()(std::byte* data) const noexcept;
};

/// Uniquely owns aligned bytes, with an initialized prefix of size() bytes.
/// Invariants: size() <= capacity(); alignment() never decreases except on move.
/// Default-constructed and moved-from objects have null data, zero size, and zero capacity.
/// Zero size may retain an allocation; reserve never reduces capacity.
/// Owns bytes, not object destructors; only bytewise-transferable payloads belong here.
/// Borrowed pointers expire on reallocation, destruction, or ownership transfer.
/// Example: `storage bytes; bytes.reserve(256, 256); bytes.resize(256);`.
class storage {
    std::unique_ptr<std::byte, aligned_delete> _data;
    std::size_t _size = 0;
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
    /// Releases prior ownership and transfers other; self-move leaves this unchanged.
    storage& operator=(storage&& other) noexcept;

    /// Ensures capacity >= bytes and alignment >= both old and requested alignment.
    /// Preserves size and initialized bytes; zero bytes need not allocate.
    /// Validates alignment as a nonzero power of two, throwing std::invalid_argument.
    /// Throws std::overflow_error for capacity arithmetic overflow and std::bad_alloc
    /// for allocation failure. Errors leave the storage unchanged.
    /// Complexity: O(size) transfer time and O(new capacity) allocation space on growth.
    void reserve(std::size_t bytes, std::size_t alignment);

    /// Sets the initialized extent, zero-initializing newly exposed bytes.
    /// Preserves the retained prefix and alignment; shrinking retains capacity.
    /// Propagates reserve errors without changing the storage.
    /// Complexity: O(size + bytes) time and O(new capacity) space if growth allocates.
    void resize(std::size_t bytes);

    /// Borrows the allocation; only the initialized prefix may be read.
    std::byte* data() noexcept { return _data.get(); }
    /// Borrows the allocation for reading the initialized prefix.
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
