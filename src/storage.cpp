// SPDX-License-Identifier: BSL-1.0
#include "storage.hpp"
#include <cel/runtime/layout.hpp>

#include <algorithm>
#include <bit>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace {

/// Validates a nonzero power-of-two alignment, throwing std::invalid_argument otherwise.
void validate_alignment(std::size_t alignment) {
    if (!std::has_single_bit(alignment))
        throw std::invalid_argument("alignment must be a nonzero power of two");
}

} // namespace

namespace cel::detail {

std::size_t checked_add(std::size_t left, std::size_t right) {
    if (right > std::numeric_limits<std::size_t>::max() - left)
        throw std::overflow_error("byte extent overflow");
    return left + right;
}

std::size_t align_offset(std::size_t offset, std::size_t alignment) {
    validate_alignment(alignment);
    const auto remainder = offset % alignment;
    return remainder == 0 ? offset : detail::checked_add(offset, alignment - remainder);
}

void aligned_delete::operator()(std::byte* data) const noexcept {
    ::operator delete(data, std::align_val_t{alignment});
}

storage::storage(storage&& other) noexcept :
    _data(std::move(other._data)), _size(std::exchange(other._size, 0)),
    _capacity(std::exchange(other._capacity, 0)) {
    other._data.get_deleter().alignment = alignof(std::max_align_t);
}

storage& storage::operator=(storage&& other) noexcept {
    if (this != &other) {
        _data = std::move(other._data);
        _size = std::exchange(other._size, 0);
        _capacity = std::exchange(other._capacity, 0);
        other._data.get_deleter().alignment = alignof(std::max_align_t);
    }
    return *this;
}

// The byte-count/alignment pair follows the storage allocation interface.
// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
void storage::reserve(std::size_t bytes, std::size_t requested_alignment) {
    validate_alignment(requested_alignment);
    const auto new_alignment = std::max(alignment(), requested_alignment);
    if (bytes <= _capacity && new_alignment == alignment()) return;
    if (bytes == 0 && _capacity == 0) {
        _data.get_deleter().alignment = new_alignment;
        return;
    }
    const auto grown_capacity =
        bytes > _capacity ? std::max(bytes, checked_add(_capacity, _capacity)) : _capacity;
    const auto new_capacity = align_offset(grown_capacity, new_alignment);
    std::unique_ptr<std::byte, aligned_delete> replacement(
        static_cast<std::byte*>(::operator new(new_capacity, std::align_val_t{new_alignment})),
        aligned_delete{new_alignment});
    if (_size != 0) std::memcpy(replacement.get(), data(), _size);
    _data = std::move(replacement);
    _capacity = new_capacity;
}

void storage::resize(std::size_t bytes) {
    reserve(bytes, alignment());
    if (bytes > _size) std::memset(data() + _size, 0, bytes - _size);
    _size = bytes;
}

} // namespace cel::detail
