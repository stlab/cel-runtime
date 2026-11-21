// SPDX-License-Identifier: BSL-1.0
#include "frame.hpp"
#include "storage.hpp"
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
#include <vector>

namespace cel::detail {
namespace {
/// A live canonical operand with retained destruction metadata.
struct stack_slot {
    value_type type;      ///< Retains the complete descriptor until payload retirement.
    std::size_t previous; ///< Restores the cursor, including preceding padding.
    std::size_t offset;   ///< Locates canonical bytes in reusable storage.
};
} // namespace
/// The exclusive state of a reusable operand stack and its evaluation scopes.
///
/// Zero-byte operands remain distinct live slots. Only the innermost frame mutates the
/// stack, and canonical phases exclude reentrant mutation, extraction, and frame entry.
struct stack_state {
    // [knowledge] slots describes exactly the initialized live owners. cursor excludes
    // spare initialized bytes; each slot.previous restores the prior byte cursor,
    // including padding. active borrows the innermost frame, and phase_active belongs
    // to the one current canonical ownership operation.
    storage bytes;                 ///< Owns bytewise-transferable canonical storage.
    std::vector<stack_slot> slots; ///< Owns exactly the initialized operand slots.
    std::size_t cursor = 0;        ///< Marks active bytes, not spare initialized storage.
    frame* active = nullptr;       ///< Borrows the innermost live frame.
    bool phase_active = false;     ///< Excludes reentry while canonical bytes/slots are unstable.
};
void frame_access::reserve_result(frame& scope, const value_type& result, std::size_t consumed) {
    scope.require_active();
    auto& data = scope._stack->state();
    if (consumed > data.slots.size() - scope._checkpoint)
        throw std::out_of_range("transfer operand underflow");
    raw_stack::phase_guard phase(*scope._stack);
    const auto remaining = data.slots.size() - consumed;
    const auto cursor = consumed == 0 ? data.cursor : data.slots[remaining].previous;
    const auto bytes = result.byte_layout();
    const auto extent = checked_add(align_offset(cursor, bytes.alignment), bytes.size);
    data.slots.reserve(checked_add(remaining, 1));
    data.bytes.reserve(extent, bytes.alignment);
    if (extent > data.bytes.size()) data.bytes.resize(extent);
}
} // namespace cel::detail

namespace cel {
using detail::align_offset;
using detail::borrowed_value;
using detail::frame;

namespace {
/// Checks that nonempty canonical input has aligned source bytes.
///
/// Any zero-byte layout accepts null; does not compare complete types or ownership pairs.
///
/// Throws std::invalid_argument for missing or misaligned nonempty bytes.
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
raw_stack::phase_guard::phase_guard(raw_stack& stack) noexcept : _state(stack._state.get()) {
    assert(!_state || !_state->phase_active);
    if (_state) {
        if (_state->phase_active) std::terminate();
        _state->phase_active = true;
    }
}
raw_stack::phase_guard::~phase_guard() noexcept {
    if (_state) _state->phase_active = false;
}
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
void raw_stack::require_idle() const {
    if (_state && _state->phase_active)
        throw std::logic_error("stack access during a canonical ownership phase");
}
void raw_stack::require_access(const frame* owner) const {
    require_idle();
    if ((_state ? _state->active : nullptr) != owner)
        throw std::logic_error("only the innermost frame may mutate this stack");
}
void raw_stack::reserve(std::size_t bytes, std::size_t alignment) {
    require_access(nullptr);
    auto& data = state();
    phase_guard phase(*this);
    data.bytes.reserve(bytes, alignment);
    data.slots.reserve(bytes);
}
void raw_stack::reserve_depth(std::size_t count) {
    require_access(nullptr);
    auto& data = state();
    phase_guard phase(*this);
    data.slots.reserve(count);
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
    (void)state();
    phase_guard phase(*this);
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
    (void)state();
    phase_guard phase(*this);
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
    phase_guard phase(*this);
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
    phase_guard phase(*this);
    const auto& slot = _state->slots.back();
    auto result = detail::value_access::take(
        slot.type, _state->bytes.data() ? _state->bytes.data() + slot.offset : nullptr);
    _state->cursor = slot.previous;
    _state->slots.pop_back();
    return result;
}
void raw_stack::release(std::size_t floor, const value_type& expected, std::byte* destination) {
    phase_guard phase(*this);
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
    stack.require_idle();
    stack._state->active = this;
}
frame::frame(raw_stack& stack, std::span<const borrowed_value> arguments, const void* context) :
    _stack(&stack), _parent(stack.state().active), _checkpoint(stack.depth()),
    _canonical_arguments(arguments), _context(context) {
    stack.require_idle();
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

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include "instruction.hpp"
#include <array>
#include <cel/runtime/detail/raw_segment.hpp>
#include <cstdint>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <vector>

namespace {
/// Exercises a client leaf requiring a larger alignment than built-ins.
struct alignas(256) aligned_leaf {
    double number; ///< Stores the test's observable client payload.
};

/// Verifies empty scope lifetime and mixed canonical layout.
TEST_CASE("empty frames and mixed leaves preserve stack order and cursor") {
    cel::raw_stack stack;
    CHECK(stack.depth() == 0);
    CHECK(stack.cursor() == 0);
    cel::detail::raw_segment empty;
    {
        cel::detail::frame scope(stack);
        empty.execute(scope);
        scope.push(true);
        scope.push(4.0);
        scope.push(aligned_leaf{9.0});
        CHECK(scope.pop<aligned_leaf>().number == 9.0);
        CHECK(scope.pop<double>() == 4.0);
        CHECK(scope.pop<bool>());
        CHECK(stack.cursor() == 0);
    }

    CHECK(stack.depth() == 0);
    stack.push(7.0);
    {
        cel::detail::frame scope(stack);
        scope.push(false);
    }
    CHECK(stack.pop<double>() == 7.0);
}

/// Verifies source operand order, zero arguments, and extensible arity.
TEST_CASE("stored operations preserve noncommutative operand order at every arity") {
    cel::raw_stack stack;
    cel::detail::raw_segment segment;
    segment.push_op0<double>([](cel::detail::frame&) { return 12.0; });
    segment.push_op0<double>([](cel::detail::frame&) { return 3.0; });
    segment.push_op2<double, double, double>(
        [](cel::detail::frame&, double a, double b) { return a / b; });
    segment.push_op1<double, double>([](cel::detail::frame&, double a) { return a + 1.0; });
    segment.push_op0<double>([](cel::detail::frame&) { return 2.0; });
    segment.push_op0<double>([](cel::detail::frame&) { return 1.0; });
    segment.push_op3<double, double, double, double>(
        [](cel::detail::frame&, double a, double b, double c) { return a - b - c; });
    cel::detail::frame scope(stack);
    segment.execute(scope);
    CHECK(scope.pop<double>() == 2.0);
    cel::detail::raw_segment arbitrary;
    arbitrary.push_op<double, double, double, double, double, double>(
        [](cel::detail::frame&, double a, double b, double c, double d, double e) {
            return a - b - c - d - e;
        });
    for (auto n : {20.0, 1.0, 2.0, 3.0, 4.0})
        scope.push(n);
    arbitrary.execute(scope);
    CHECK(scope.pop<double>() == 10.0);
    cel::detail::raw_segment unit;
    unit.push_op0<void>([](cel::detail::frame&) {});
    unit.execute(scope);
    CHECK(scope.depth() == 0);
}

/// Owns a nontrivial capture whose address must remain stable after construction.
struct alignas(256) owning_capture {
    std::unique_ptr<int> owner; ///< Owns a callback resource.
    int* released;              ///< Borrows the release counter until segment destruction.
    mutable const owning_capture* address = nullptr; ///< Records first execution address.
    /// Acquires one exclusive resource.
    explicit owning_capture(int& count) : owner(std::make_unique<int>(8)), released(&count) {}
    /// Transfers construction ownership normally.
    owning_capture(owning_capture&&) noexcept = default;
    /// Counts only destruction of the final resource owner.
    ~owning_capture() {
        if (owner) ++*released;
    }
    /// Returns the captured number, rejecting relocation after the first call.
    double operator()(cel::detail::frame&) const {
        if (address && address != this) throw std::runtime_error("relocated capture");
        if (reinterpret_cast<std::uintptr_t>(this) % alignof(owning_capture) != 0)
            throw std::runtime_error("misaligned capture");
        address = this;
        return *owner;
    }
};

/// Supplies a copyable, nonmovable over-aligned callback with independent state.
struct alignas(256) copy_only_capture {
    double number; ///< Owns the callback's result.
    /// Initializes the result.
    explicit copy_only_capture(double input) : number(input) {}
    /// Copies the result independently.
    copy_only_capture(const copy_only_capture&) = default;
    /// Disallows move construction.
    copy_only_capture(copy_only_capture&&) = delete;
    /// Returns the owned result.
    double operator()(cel::detail::frame&) const { return number; }
};

TEST_CASE("raw operations copy nonmovable lvalue captures independently") {
    copy_only_capture input(7.0);
    cel::detail::raw_segment segment;
    segment.push_op0<double>(input);
    input.number = 99.0;
    cel::raw_stack stack;
    cel::detail::frame scope(stack);
    segment.execute(scope);
    CHECK(scope.pop<double>() == 7.0);
}

/// Verifies independent instruction ownership and stable nontrivial capture addresses.
TEST_CASE("captures survive input owners segment growth and segment moves") {
    int released = 0;
    cel::raw_stack stack;
    {
        cel::detail::raw_segment segment;
        segment.push_op0<double>(owning_capture(released));
        {
            cel::detail::frame scope(stack);
            segment.execute(scope);
            CHECK(scope.pop<double>() == 8.0);
        }
        for (int i = 0; i != 200; ++i)
            segment.push_op0<void>([](cel::detail::frame&) {});
        cel::detail::raw_segment moved(std::move(segment));
        cel::detail::frame scope(stack);
        moved.execute(scope);
        CHECK(scope.pop<double>() == 8.0);
        CHECK(released == 0);
    }
    CHECK(released == 1);
}

/// Verifies reservation covers both payload bytes and active slot bookkeeping.
TEST_CASE("reserved scalar execution reuses one stack") {
    cel::raw_stack stack;
    stack.reserve(1024, 256);
    cel::detail::raw_segment number;
    number.push_op0<double>([](cel::detail::frame&) { return 6.0; });
    number.push_op1<double, double>([](cel::detail::frame&, double n) { return n / 2.0; });
    cel::detail::raw_segment boolean;
    boolean.push_op0<bool>([](cel::detail::frame&) { return true; });
    boolean.push_op1<bool, bool>([](cel::detail::frame&, bool n) { return !n; });
    {
        cel::detail::frame scope(stack);
        number.execute(scope);
        CHECK(scope.pop<double>() == 3.0);
        boolean.execute(scope);
        CHECK_FALSE(scope.pop<bool>());
    }
    bool correct = true;
    {
        for (int i = 0; i != 2000; ++i) {
            cel::detail::frame scope(stack);
            number.execute(scope);
            correct = correct && scope.pop<double>() == 3.0;
            boolean.execute(scope);
            correct = correct && !scope.pop<bool>();
        }
    }
    CHECK(correct);
    CHECK(stack.depth() == 0);
}

/// Verifies explicit arguments/context and nested suffix isolation.
TEST_CASE("nested evaluations preserve outer arguments and live temporaries") {
    cel::raw_stack stack;
    const std::array arguments{cel::value::from(11.0)};
    int context = 7;
    cel::detail::raw_segment nested;
    nested.push_op0<double>(
        [](cel::detail::frame& scope) { return scope.argument(0).as<double>(); });
    cel::detail::raw_segment outer;
    outer.push_op1<double, double>([&](cel::detail::frame& scope, double input) {
        scope.push(5.0);
        cel::detail::frame inner(stack, scope.arguments(), scope.context());
        nested.execute(inner);
        const auto result = inner.pop<double>();
        CHECK(scope.argument(0).as<double>() == 11.0);
        CHECK(scope.context() == &context);
        return result + input;
    });
    cel::detail::frame scope(stack, arguments, &context);
    scope.push(3.0);
    outer.execute(scope);
    CHECK(scope.pop<double>() == 14.0);
    CHECK(scope.pop<double>() == 5.0);
    CHECK_THROWS_AS(scope.argument(1), std::out_of_range);
}

/// Counts a synthetic opaque canonical resource independently of outer byte storage.
int live_resources = 0;
/// Selects which clone throws after earlier suffix ownership has been acquired.
int clones_before_failure = -1;
/// Clones one synthetic carrier, throwing without leaving partial ownership.
void clone_resource(const std::byte* source, std::byte* target) {
    if (clones_before_failure == 0) throw std::runtime_error("clone failed");
    if (clones_before_failure > 0) --clones_before_failure;
    std::memcpy(target, source, sizeof(void*));
    ++live_resources;
}
/// Releases one synthetic owner without allocating.
void destroy_resource(std::byte*) noexcept { --live_resources; }

/// Verifies frame boundaries report malformed external requests before accessing memory.
TEST_CASE("checked stack boundaries reject underflow type mismatch and active-frame mutation") {
    cel::raw_stack stack;
    CHECK_THROWS_AS(stack.pop<bool>(), std::out_of_range);
    CHECK_THROWS_AS(stack.reserve(10, 3), std::invalid_argument);
    stack.push(2.0);
    CHECK_THROWS_AS(stack.pop<bool>(), std::invalid_argument);
    CHECK_THROWS_AS(stack.pop<std::uint64_t>(), std::invalid_argument);
    CHECK(stack.depth() == 1);
    {
        cel::detail::frame scope(stack);
        CHECK_THROWS_AS(scope.pop<double>(), std::out_of_range);
        CHECK_THROWS_AS(stack.clear(), std::logic_error);
        CHECK_THROWS_AS(stack.push(true), std::logic_error);
        CHECK_THROWS_AS(stack.reserve(1024, 256), std::logic_error);
        CHECK_THROWS_AS(stack.reserve_depth(1024), std::logic_error);
        CHECK_THROWS_AS(cel::raw_stack(std::move(stack)), std::logic_error);
        cel::raw_stack destination;
        // NOLINTNEXTLINE(bugprone-use-after-move): rejected move preserves source.
        CHECK_THROWS_AS(destination = std::move(stack), std::logic_error);
        // NOLINTNEXTLINE(bugprone-use-after-move): rejected moves preserve frames.
        cel::detail::frame inner(stack);
        CHECK_THROWS_AS(scope.push(true), std::logic_error);
    }
    CHECK(stack.pop<double>() == 2.0);
    stack.clear();
}

/// Verifies preparation of zero-byte slots, reusable moved-from stacks, and segment assignment.
TEST_CASE("zero byte units and over-aligned operations reuse explicitly reserved depth") {
    cel::raw_stack stack;
    stack.reserve(1024, 256);
    stack.reserve_depth(200);
    cel::detail::raw_segment segment;
    segment.push_op0<aligned_leaf>([](cel::detail::frame&) { return aligned_leaf{13.0}; });
    segment.push_op0<std::tuple<>>([](cel::detail::frame&) { return std::tuple{}; });
    {
        cel::detail::frame scope(stack);
        segment.execute(scope);
        CHECK(scope.pop<std::tuple<>>() == std::tuple{});
        CHECK(scope.pop<aligned_leaf>().number == 13.0);
        CHECK(&scope.stack() == &stack);
    }
    bool correct = true;
    {
        for (int i = 0; i != 100; ++i) {
            cel::detail::frame scope(stack);
            segment.execute(scope);
            correct = correct && scope.pop<std::tuple<>>() == std::tuple{};
            correct = correct && scope.pop<aligned_leaf>().number == 13.0;
            for (int j = 0; j != 200; ++j)
                scope.push(std::tuple{});
        }
    }
    CHECK(correct);
    int released = 0;
    {
        cel::detail::raw_segment replaced;
        replaced.push_op0<double>(owning_capture(released));
        replaced = std::move(segment);
        CHECK(released == 1);
        cel::detail::frame scope(stack);
        replaced.execute(scope);
        CHECK(scope.pop<std::tuple<>>() == std::tuple{});
        CHECK(scope.pop<aligned_leaf>().number == 13.0);
    }
    cel::raw_stack moved(std::move(stack));
    // NOLINTNEXTLINE(bugprone-use-after-move): the public contract promises reusable source stacks.
    stack.push(17.0);
    // NOLINTNEXTLINE(bugprone-use-after-move): observes the promised reused source.
    CHECK(stack.pop<double>() == 17.0);
    CHECK(moved.depth() == 0);
}

/// Verifies the canonical arbitrary-arity bridge and clone exceptions under execute rollback.
TEST_CASE("canonical instruction captures retain descriptors and clean failed clone suffixes") {
    const auto opaque = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<bool>()),
        {clone_resource, destroy_resource});
    std::array<std::byte, sizeof(void*)> bytes{};
    auto child = cel::detail::value_access::copy(opaque, bytes.data());
    auto aggregate = cel::value::tuple(std::array{child, child});
    const auto baseline = live_resources;
    cel::raw_stack stack;
    stack.push_clone(child);
    {
        cel::detail::raw_segment sequence;
        cel::detail::segment_access::append(sequence, [owner = std::move(aggregate)](
                                                          cel::detail::frame& scope) {
            cel::detail::frame_access::push(
                scope, owner.type(), cel::detail::value_access::data(owner),
                [](const void* source, const cel::value_type& type, std::byte* target) {
                    cel::detail::clone_payload(type, static_cast<const std::byte*>(source), target);
                });
        });
        // A separate matching bridge sequence demonstrates its unchecked signature contract.
        cel::detail::raw_segment scalar;
        cel::detail::segment_access::append(scalar, [](cel::detail::frame& scope) {
            const auto type = cel::value_type::leaf<double>();
            const auto* operand = cel::detail::frame_access::operand(scope, 0, type);
            double result{};
            std::memcpy(&result, operand, sizeof(result));
            cel::detail::frame_access::discard(scope, 1);
            scope.push(result / 2.0);
        });
        cel::detail::frame scope(stack);
        scope.push(12.0);
        scalar.execute(scope);
        CHECK(scope.pop<double>() == 6.0);
        scope.push(std::vector{1.0, 2.0});
        clones_before_failure = 1;
        CHECK_THROWS_AS(sequence.execute(scope), std::runtime_error);
        clones_before_failure = -1;
        CHECK(scope.depth() == 0);
        CHECK(live_resources == baseline + 1);
        scope.push(18.0);
        scalar.execute(scope);
        CHECK(scope.pop<double>() == 9.0);
    }
    CHECK(stack.depth() == 1);
    stack.clear();
}
/// Verifies throwing callbacks and clone failures retire only the acquired suffix.
TEST_CASE("throwing execution and composite cloning preserve the live prefix") {
    cel::raw_stack stack;
    auto prefix = cel::value::from(std::vector<std::vector<double>>{{1.0, 2.0}, {3.0}});
    stack.push_take(prefix);
    CHECK(prefix.type().kind() == cel::type_kind::tuple);
    cel::detail::raw_segment failure;
    failure.push_op0<std::vector<double>>(
        [](cel::detail::frame&) { return std::vector{7.0, 8.0}; });
    failure.push_op1<double, std::vector<double>>(
        [](cel::detail::frame& scope, std::vector<double>) -> double {
            scope.push(std::vector{9.0});
            throw std::runtime_error("callback failed");
        });
    {
        cel::detail::frame scope(stack);
        CHECK_THROWS_AS(failure.execute(scope), std::runtime_error);
        CHECK(scope.depth() == 0);
        scope.push(true);
        CHECK(scope.pop<bool>());
    }
    CHECK(stack.depth() == 1);
    CHECK(stack.pop<std::vector<std::vector<double>>>() ==
          std::vector<std::vector<double>>{{1.0, 2.0}, {3.0}});

    const auto opaque = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<bool>()),
        {clone_resource, destroy_resource});
    std::array<std::byte, sizeof(void*)> bytes{};
    auto child = cel::detail::value_access::copy(opaque, bytes.data());
    const std::array children{child, child};
    auto owned = cel::value::tuple(children);
    const auto baseline = live_resources;
    stack.push(42.0);
    {
        cel::detail::frame scope(stack);
        scope.push_clone(owned);
        clones_before_failure = 1;
        CHECK_THROWS_AS(scope.push_clone(owned), std::runtime_error);
        CHECK(scope.depth() == 1);
        CHECK(live_resources == baseline + 2);
        clones_before_failure = -1;
    }
    CHECK(live_resources == baseline);
    CHECK(stack.pop<double>() == 42.0);
}
/// Verifies borrowed canonical input slots are read lazily and results transfer without boxes.
TEST_CASE("explicit canonical frames clone emitted reads and extract independent results") {
    const auto scalar_type = cel::value_type::leaf<double>();
    double input = 23.0;
    const std::array scalar_arguments{
        cel::detail::borrowed_value{scalar_type, reinterpret_cast<const std::byte*>(&input)}};
    cel::detail::raw_segment reads;
    cel::detail::segment_access::append(reads,
                                        [](cel::detail::frame& scope) { scope.push_argument(0); });
    cel::raw_stack stack;
    stack.reserve(1024, 256);
    int context = 5;
    double output = 0;
    {
        cel::detail::frame scope(stack, scalar_arguments, &context);
        reads.execute(scope);
        cel::detail::frame_access::release_result(scope, scalar_type,
                                                  reinterpret_cast<std::byte*>(&output));
        CHECK(output == 23.0);
        CHECK(scope.depth() == 0);
        CHECK(scope.argument(0).as<double>() == 23.0);
    }
    bool correct = true;
    {
        for (int i = 0; i != 1000; ++i) {
            cel::detail::frame scope(stack, scalar_arguments, &context);
            reads.execute(scope);
            cel::detail::frame nested(stack, scope.canonical_arguments(), scope.context());
            reads.execute(nested);
            cel::detail::frame_access::release_result(nested, scalar_type,
                                                      reinterpret_cast<std::byte*>(&output));
            correct = correct && output == 23.0;
        }
    }
    CHECK(correct);

    auto owner = cel::value::from(std::vector<std::vector<double>>{{1.0, 2.0}});
    const std::array borrowed{
        cel::detail::borrowed_value{owner.type(), cel::detail::value_access::data(owner)}};
    {
        cel::detail::frame scope(stack, borrowed);
        reads.execute(scope);
        auto copy = scope.pop_value();
        copy.set(0, cel::value::from(std::vector{8.0}));
        CHECK(owner.at(0).at(0).as<double>() == 1.0);
        CHECK(copy.at(0).at(0).as<double>() == 8.0);
        CHECK_THROWS_AS(scope.push_argument(1), std::out_of_range);
        cel::detail::storage result;
        const auto layout = owner.type().byte_layout();
        result.reserve(layout.size, layout.alignment);
        result.resize(layout.size);
        reads.execute(scope);
        cel::detail::frame_access::release_result(scope, owner.type(), result.data());
        owner.set(0, cel::value::from(std::vector{6.0}));
        auto independent = cel::detail::value_access::take(owner.type(), result.data());
        CHECK(independent.at(0).at(0).as<double>() == 1.0);
        CHECK(owner.at(0).at(0).as<double>() == 6.0);
    }
    const std::array missing{cel::detail::borrowed_value{scalar_type, nullptr}};
    CHECK_THROWS_AS(cel::detail::frame(stack, missing), std::invalid_argument);
    alignas(double) std::array<std::byte, sizeof(double) + alignof(double)> unaligned{};
    const std::array misaligned{cel::detail::borrowed_value{scalar_type, unaligned.data() + 1}};
    CHECK_THROWS_AS(cel::detail::frame(stack, misaligned), std::invalid_argument);
    cel::detail::frame scope(stack);
    CHECK(scope.depth() == 0);
}
/// Records bounded hook reentry attempts without relying on an invalid raw signature.
struct reentry_state {
    cel::raw_stack& stack;        ///< Borrows the tested stack.
    cel::detail::frame* scope;    ///< Borrows the active scope, or null during direct clear.
    int rejected = 0;             ///< Counts documented logic-error rejections.
    bool unexpected = false;      ///< Records any other exception from noexcept destruction probes.
    bool propagate = false;       ///< Lets a clone rejection escape to test rollback.
    bool clone_attempted = false; ///< Bounds clone reentry to one attempt.
    bool destroy_attempted = false; ///< Bounds retirement reentry to one attempt.
    /// Attempts same-owner mutation and both forms of nested frame entry.
    void probe() {
        try {
            if (scope)
                scope->push(99.0);
            else
                stack.push(99.0);
        } catch (const std::logic_error&) {
            ++rejected;
            if (propagate) throw;
        }
        try {
            cel::detail::frame nested(stack);
        } catch (const std::logic_error&) {
            ++rejected;
        }
        try {
            cel::detail::frame nested(stack, std::span<const cel::detail::borrowed_value>{});
        } catch (const std::logic_error&) {
            ++rejected;
        }
    }
};
/// Enables hooks only during their bounded test scopes, never as production context.
reentry_state* active_reentry = nullptr;
/// Restores the test hook context before its borrowed state dies, including on exceptions.
struct reentry_scope {
    reentry_state* previous; ///< Retains the prior single-threaded hook context.
    /// Borrows the state for this bounded probe scope.
    explicit reentry_scope(reentry_state& state) : previous(active_reentry) {
        active_reentry = &state;
    }
    /// Disallows duplicated hook-scope ownership.
    reentry_scope(const reentry_scope&) = delete;
    /// Disallows reassigned hook-scope ownership.
    reentry_scope& operator=(const reentry_scope&) = delete;
    /// Restores the prior context.
    ~reentry_scope() { active_reentry = previous; }
};
/// Clones a synthetic owner after a bounded construction reentry attempt.
void clone_reentry(const std::byte* source, std::byte* target) {
    if (active_reentry && !active_reentry->clone_attempted) {
        active_reentry->clone_attempted = true;
        active_reentry->probe();
    }
    std::memcpy(target, source, sizeof(void*));
    ++live_resources;
}
/// Retires a synthetic owner while catching rejection inside its noexcept hook.
void destroy_reentry(std::byte*) noexcept {
    if (active_reentry && !active_reentry->destroy_attempted) {
        active_reentry->destroy_attempted = true;
        try {
            active_reentry->probe();
        } catch (...) {
            active_reentry->unexpected = true;
        }
    }
    --live_resources;
}
/// Catches clone/write offset reuse and retiring-slot mutation at safe boundaries.
TEST_CASE("payload clone and noexcept retirement hooks reject reentry") {
    const auto type = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<double>()),
        {clone_reentry, destroy_reentry});
    std::array<std::byte, sizeof(void*)> bytes{};
    auto source = cel::detail::value_access::copy(type, bytes.data());
    const auto baseline = live_resources;
    cel::raw_stack stack;
    stack.reserve(1024, 256);
    stack.push(17.0);
    {
        cel::detail::frame scope(stack);
        reentry_state attempts{stack, &scope};
        reentry_scope enabled(attempts);
        scope.push_clone(source);
        CHECK(attempts.rejected == 3);
        CHECK(scope.depth() == 1);
        cel::detail::frame_access::discard(scope, scope.depth());
        CHECK(attempts.rejected == 6);
        CHECK_FALSE(attempts.unexpected);
        active_reentry = nullptr;
        CHECK(live_resources == baseline);
        scope.push(8.0);
        CHECK(scope.pop<double>() == 8.0);
    }
    CHECK(stack.pop<double>() == 17.0);
    stack.push_clone(source);
    reentry_state direct{stack, nullptr};
    direct.clone_attempted = true;
    reentry_scope enabled_direct(direct);
    stack.clear();
    active_reentry = nullptr;
    CHECK(direct.rejected == 3);
    CHECK_FALSE(direct.unexpected);
    CHECK(stack.depth() == 0);
    stack.push(true);
    CHECK(stack.pop<bool>());
}

/// Catches a gate left held after a rejected clone callback unwinds through execute.
TEST_CASE("rejected clone reentry rolls back only the frame suffix and restores access") {
    const auto type = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<double>()),
        {clone_reentry, destroy_reentry});
    std::array<std::byte, sizeof(void*)> bytes{};
    auto source = cel::detail::value_access::copy(type, bytes.data());
    cel::raw_stack stack;
    stack.reserve(1024, 256);
    stack.push(std::vector{4.0, 5.0});
    cel::detail::raw_segment sequence;
    cel::detail::segment_access::append(
        sequence, [&source](cel::detail::frame& scope) { scope.push_clone(source); });
    {
        cel::detail::frame scope(stack);
        scope.push(std::vector{9.0});
        reentry_state attempts{stack, &scope};
        attempts.propagate = true;
        reentry_scope enabled(attempts);
        CHECK_THROWS_AS(sequence.execute(scope), std::logic_error);
        active_reentry = nullptr;
        CHECK(scope.depth() == 0);
        scope.push(7.0);
        CHECK(scope.pop<double>() == 7.0);
        cel::detail::frame nested(stack);
        nested.push(false);
        CHECK_FALSE(nested.pop<bool>());
    }
    CHECK(stack.pop<std::vector<double>>() == std::vector{4.0, 5.0});
}

/// Verifies null bytes preserve non-unit zero-byte aggregate identity and stack effects.
TEST_CASE("borrowed non-unit zero-byte aggregates preserve complete shape") {
    using aggregate = std::tuple<std::tuple<>, std::tuple<>>;
    const auto type = cel::detail::concrete_adapter<aggregate>::type();
    const std::array inputs{cel::detail::borrowed_value{type, nullptr}};
    cel::raw_stack stack;
    cel::detail::frame scope(stack, inputs);
    CHECK(type.byte_layout().size == 0);
    CHECK(type.fields().size() == 2);
    CHECK_FALSE(type.same_shape(cel::value_type{}));
    CHECK(scope.argument(0).as<aggregate>() == aggregate{});
    scope.push_argument(0);
    auto owner = scope.pop_value();
    CHECK(owner.type().same_shape(type));
    CHECK(owner.size() == 2);
    scope.push_take(owner);
    CHECK(scope.pop<aggregate>() == aggregate{});
    CHECK(scope.depth() == 0);
    CHECK(stack.cursor() == 0);
}

/// Verifies allocating raw reads share the phase gate and release it on reader failure.
TEST_CASE("scoped canonical acquisition and direct writers contain reentry") {
    cel::raw_stack stack;
    stack.reserve(1024, 256);
    stack.push(15.0);
    {
        cel::detail::frame scope(stack);
        scope.push(std::vector{7.0, 8.0});
        const auto type = cel::detail::concrete_adapter<std::vector<double>>::type();
        reentry_state attempts{stack, &scope};
        reentry_scope enabled(attempts);
        const auto acquired = cel::detail::frame_access::acquire(scope, [&](const auto& operands) {
            attempts.probe();
            return cel::detail::value_access::copy(type, operands.operand(0, type));
        });
        active_reentry = nullptr;
        CHECK(attempts.rejected == 3);
        CHECK(acquired.as<std::vector<double>>() == std::vector{7.0, 8.0});
        CHECK(scope.depth() == 1);
        CHECK_THROWS_AS(cel::detail::frame_access::acquire(scope,
                                                           [&](const auto& operands) -> double {
                                                               (void)operands.operand(0, type);
                                                               throw std::runtime_error(
                                                                   "reader failed");
                                                           }),
                        std::runtime_error);
        CHECK(scope.pop<std::vector<double>>() == std::vector{7.0, 8.0});
        reentry_state writer{stack, &scope};
        reentry_scope enabled_writer(writer);
        const auto scalar_type = cel::value_type::leaf<double>();
        const double input = 3.0;
        cel::detail::frame_access::push(
            scope, scalar_type, &input,
            [](const void* source, const cel::value_type&, std::byte* target) {
                active_reentry->probe();
                std::memcpy(target, source, sizeof(double));
            });
        active_reentry = nullptr;
        CHECK(writer.rejected == 3);
        CHECK(scope.pop<double>() == 3.0);
        cel::detail::frame nested(stack);
        nested.push(false);
        CHECK_FALSE(nested.pop<bool>());
    }
    CHECK(stack.pop<double>() == 15.0);
}
} // namespace

#endif
