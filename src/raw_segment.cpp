// SPDX-License-Identifier: BSL-1.0
#include "instruction.hpp"
#include <cel/runtime/detail/raw_segment.hpp>
#include <utility>

namespace cel::detail {
raw_segment::raw_segment() noexcept = default;
raw_segment::~raw_segment() = default;
raw_segment::raw_segment(raw_segment&& other) noexcept = default;
raw_segment& raw_segment::operator=(raw_segment&& other) noexcept {
    if (this != &other) _storage = std::move(other._storage);
    return *this;
}
void* raw_segment::allocate_capture(std::size_t size, std::size_t alignment) {
    if (!_storage)
        _storage =
            detail::retirement_ptr<detail::instruction_storage>(new detail::instruction_storage());
    return _storage->arena.allocate(size, alignment);
}
void raw_segment::append(const void* capture,
                         void (*execute)(const void*, frame&),
                         void (*destroy)(const void*) noexcept,
                         void (*retire)(const void*, detail::retirement_queue&) noexcept) {
    _storage->instructions.push_back({capture, execute, destroy, retire});
}
void raw_segment::execute(frame& scope) const {
    scope.require_active();
    try {
        if (!_storage) return;
        // Execute immutable dispatch records in stored order.
        for (const auto& instruction : _storage->instructions)
            instruction.execute(instruction.capture, scope);
    } catch (...) {
        scope.reset();
        throw;
    }
}
} // namespace cel::detail
