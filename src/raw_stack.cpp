// SPDX-License-Identifier: BSL-1.0
#include "frame.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <algorithm>
#include <cassert>
#include <cel/runtime/raw_stack.hpp>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <stdexcept>
#include <utility>

namespace cel {
namespace {
/// Rejects missing or misaligned borrowed bytes without traversing metadata.
void require_bytes(const borrowed_value& input) {
    const auto layout = input.type.byte_layout();
    if (layout.size != 0 &&
        (!input.data || reinterpret_cast<std::uintptr_t>(input.data) % layout.alignment != 0))
        throw std::invalid_argument("missing or misaligned canonical input bytes");
}
} // namespace
void borrowed_value::require_type(const value_type& expected) const {
    require_bytes(*this);
    if (!detail::type_access::same_plan(expected, type)) detail::require_compatible(expected, type);
}
raw_stack::raw_stack() : _state(std::make_unique<detail::stack_state>()) {}
raw_stack::~raw_stack() {
    if (_state && _state->active) std::terminate();
    truncate(0);
}
// NOLINTNEXTLINE(bugprone-exception-escape): move rejects active frames.
raw_stack::raw_stack(raw_stack&& other) {
    other.require_access(nullptr);
    _state = std::move(other._state);
}
// NOLINTNEXTLINE(bugprone-exception-escape): move rejects active frames.
raw_stack& raw_stack::operator=(raw_stack&& other) {
    require_access(nullptr);
    other.require_access(nullptr);
    if (this != &other) {
        truncate(0);
        _state = std::move(other._state);
    }
    return *this;
}
detail::stack_state& raw_stack::state() {
    if (!_state) _state = std::make_unique<detail::stack_state>();
    return *_state;
}
void raw_stack::require_access(const frame* owner) const {
    if ((_state ? _state->active : nullptr) != owner)
        throw std::logic_error("only the innermost frame may mutate this stack");
}
void raw_stack::reserve(std::size_t bytes, std::size_t alignment) {
    require_access(nullptr);
    auto& data = state();
    data.bytes.reserve(bytes, alignment);
    data.slots.reserve(bytes);
}
void raw_stack::reserve_depth(std::size_t count) {
    require_access(nullptr);
    state().slots.reserve(count);
}
std::size_t raw_stack::depth() const noexcept { return _state ? _state->slots.size() : 0; }
std::size_t raw_stack::cursor() const noexcept { return _state ? _state->cursor : 0; }
void raw_stack::clear() {
    require_access(nullptr);
    truncate(0);
}
std::size_t raw_stack::prepare(const value_type& type) {
    auto& data = state();
    const auto layout = type.byte_layout();
    const auto offset = align_offset(data.cursor, layout.alignment);
    if (offset > std::numeric_limits<std::size_t>::max() - layout.size)
        throw std::overflow_error("stack canonical extent overflow");
    if (data.slots.size() == data.slots.max_size())
        throw std::overflow_error("stack depth overflow");
    if (data.slots.size() == data.slots.capacity()) {
        const auto capacity = data.slots.capacity();
        const auto grown =
            capacity > data.slots.max_size() / 2 ? data.slots.max_size() : capacity * 2;
        data.slots.reserve(capacity == 0 ? 8 : grown);
    }
    const auto end = offset + layout.size;
    data.bytes.reserve(end, layout.alignment);
    if (end > data.bytes.size()) data.bytes.resize(end);
    return offset;
}
// NOLINTNEXTLINE(bugprone-exception-escape): prepare reserves slot capacity.
void raw_stack::publish(const value_type& type, std::size_t offset) noexcept {
    auto& data = *_state;
    assert(data.slots.size() < data.slots.capacity());
    data.slots.push_back({type, data.cursor, offset});
    data.cursor = offset + type.byte_layout().size;
}
void raw_stack::push_written(const value_type& type,
                             const void* source,
                             void (*write)(const void*, const value_type&, std::byte*)) {
    const auto offset = prepare(type);
    auto* destination = _state->bytes.data();
    write(source, type, destination ? destination + offset : nullptr);
    publish(type, offset);
}
void raw_stack::clone(const value& source) {
    push_written(source.type(), detail::value_access::data(source),
                 [](const void* input, const value_type& type, std::byte* destination) {
                     detail::clone_payload(type, static_cast<const std::byte*>(input), destination);
                 });
}
void raw_stack::take(value& source) {
    const auto type = source.type();
    const auto offset = prepare(type);
    auto* destination = _state->bytes.data();
    detail::value_access::release_to(source, destination ? destination + offset : nullptr);
    publish(type, offset);
}
const std::byte* raw_stack::borrow_trusted(std::size_t floor,
                                           std::size_t from_top,
                                           const value_type& expected) const {
    if (floor > depth() || from_top >= depth() - floor)
        throw std::out_of_range("operand underflow");
    const auto& slot = _state->slots[depth() - 1 - from_top];
    const auto actual = slot.type.byte_layout();
    const auto wanted = expected.byte_layout();
    if (actual.size != wanted.size || actual.alignment != wanted.alignment)
        throw std::invalid_argument("operand layout mismatch");
    return _state->bytes.data() ? _state->bytes.data() + slot.offset : nullptr;
}
const std::byte* raw_stack::borrow(std::size_t floor,
                                   std::size_t from_top,
                                   const value_type& expected) const {
    const auto* bytes = borrow_trusted(floor, from_top, expected);
    const auto& actual = _state->slots[depth() - 1 - from_top].type;
    if (!detail::type_access::same_plan(expected, actual))
        detail::require_compatible(expected, actual);
    return bytes;
}
void raw_stack::truncate(std::size_t floor) noexcept {
    assert(floor <= depth());
    if (!_state) return;
    auto& data = *_state;
    // Retire owners in reverse stack order, retaining metadata until each owner dies.
    while (data.slots.size() > floor) {
        auto& slot = data.slots.back();
        detail::destroy_payload(slot.type,
                                data.bytes.data() ? data.bytes.data() + slot.offset : nullptr);
        data.cursor = slot.previous;
        data.slots.pop_back();
    }
}
value raw_stack::extract(std::size_t floor) {
    if (depth() <= floor) throw std::out_of_range("operand underflow");
    const auto& slot = _state->slots.back();
    auto result = detail::value_access::take(
        slot.type, _state->bytes.data() ? _state->bytes.data() + slot.offset : nullptr);
    _state->cursor = slot.previous;
    _state->slots.pop_back();
    return result;
}
void raw_stack::release(std::size_t floor, const value_type& expected, std::byte* destination) {
    const auto* source = borrow_trusted(floor, 0, expected);
    const auto size = expected.byte_layout().size;
    if (size != 0) std::memcpy(destination, source, size);
    _state->cursor = _state->slots.back().previous;
    _state->slots.pop_back();
}
void raw_stack::push_clone(const value& source) {
    require_access(nullptr);
    clone(source);
}
void raw_stack::push_take(value& source) {
    require_access(nullptr);
    take(source);
}
value raw_stack::pop_value() {
    require_access(nullptr);
    return extract(0);
}
frame::frame(raw_stack& stack, std::span<const value> arguments, const void* context) :
    _stack(&stack), _parent(stack.state().active), _checkpoint(stack.depth()),
    _arguments(arguments), _context(context) {
    stack._state->active = this;
}
frame::frame(raw_stack& stack, std::span<const borrowed_value> arguments, const void* context) :
    _stack(&stack), _parent(stack.state().active), _checkpoint(stack.depth()),
    _canonical_arguments(arguments), _context(context) {
    std::ranges::for_each(arguments, require_bytes);
    stack._state->active = this;
}
frame::~frame() {
    if (_stack->_state->active != this) std::terminate();
    reset();
    _stack->_state->active = _parent;
}
void frame::require_active() const { _stack->require_access(this); }
void frame::reset() noexcept { _stack->truncate(_checkpoint); }
std::size_t frame::depth() const noexcept { return _stack->depth() - _checkpoint; }
borrowed_value frame::argument(std::size_t index) const {
    if (!_canonical_arguments.empty()) {
        if (index >= _canonical_arguments.size())
            throw std::out_of_range("argument index out of range");
        return _canonical_arguments[index];
    }
    if (index >= _arguments.size()) throw std::out_of_range("argument index out of range");
    const auto& source = _arguments[index];
    return {source.type(), detail::value_access::data(source)};
}
void frame::push_argument(std::size_t index) {
    require_active();
    const auto input = argument(index);
    _stack->push_written(
        input.type, input.data, [](const void* source, const value_type& type, std::byte* target) {
            detail::clone_payload(type, static_cast<const std::byte*>(source), target);
        });
}
void frame::discard(std::size_t count) {
    if (count > depth()) throw std::out_of_range("operand underflow");
    _stack->truncate(_stack->depth() - count);
}
void frame::push_clone(const value& source) {
    require_active();
    _stack->clone(source);
}
void frame::push_take(value& source) {
    require_active();
    _stack->take(source);
}
value frame::pop_value() {
    require_active();
    return _stack->extract(_checkpoint);
}
} // namespace cel
