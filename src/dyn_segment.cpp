// SPDX-License-Identifier: BSL-1.0
#include "callable_access.hpp"
#include "canonical.hpp"
#include "frame.hpp"
#include "instruction.hpp"
#include "storage.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <algorithm>
#include <cassert>
#include <cel/runtime/dyn_segment.hpp>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace cel::detail {
/// A frozen program with complete boundary types and storage requirements.
///
/// Shared handles have reference semantics; captures are never shallow value-copied.
struct frozen_program : retirement_node {
    raw_segment body;                 ///< Owns stable captures and const dispatch records.
    std::vector<argument_read> reads; ///< Retains each declared positional clone requirement.
    value_type result;                ///< Retains the single final owner plan.
    stack_requirements requirements;  ///< Retains statically known canonical frame peaks.
    stack_origin origin;              ///< Retains independent/dependent execution identity.
    /// Takes ownership of checked code and its complete boundary metadata.
    frozen_program(raw_segment input,
                   std::vector<argument_read> arguments,
                   value_type output,
                   stack_requirements required,
                   stack_origin ambient) :
        retirement_node(dispose_program), body(std::move(input)), reads(std::move(arguments)),
        result(std::move(output)), requirements(required), origin(ambient) {}
    /// Retires this program's boundary metadata, deferring capture retirement.
    ///
    /// \pre base is the live frozen_program selected by its retirement callback.
    ///
    /// Complexity: O(read count + released metadata) time; queued captures have their own costs.
    static void dispose_program(retirement_node& base, retirement_queue& queue) noexcept {
        auto& program = static_cast<frozen_program&>(base);
        segment_access::retire(program.body, queue);
        delete &program;
    }
};
namespace {
/// An owned literal that supplies independent results under value's copy semantics.
struct literal_instruction {
    value literal; ///< Owns one complete literal, independent of execution results.
    /// Pushes an independent clone onto the caller's frame.
    ///
    /// Complexity: O(canonical payload and ownership work).
    void operator()(frame& scope) const { scope.push_clone(literal); }
    /// Releases literal ownership into the enclosing retirement operation without allocating.
    ///
    /// Complexity: canonical ownership traversal and released literal metadata.
    void retire(retirement_queue& queue) noexcept { value_access::retire(literal, queue); }
};
/// A conditional with shared immutable true and false bodies.
struct branch_instruction {
    retirement_ptr<const frozen_program> yes; ///< Retains the selected-true body.
    retirement_ptr<const frozen_program> no;  ///< Retains the selected-false body.
    /// Executes only the body selected by the consumed boolean.
    ///
    /// Complexity: O(selected body execution work).
    void operator()(frame& scope) const {
        const bool condition = scope.pop<bool>();
        (condition ? yes->body : no->body).execute(scope);
    }
    /// Releases both body references into the supplied retirement operation.
    void retire(retirement_queue& queue) noexcept {
        yes.retire(queue);
        no.retire(queue);
    }
};
} // namespace
void segment_access::retire(cel::compiled_segment& segment, retirement_queue& queue) noexcept {
    segment._program.retire(queue);
}
void segment_access::retire(cel::detail::prepared_raw_segment& segment,
                            retirement_queue& queue) noexcept {
    segment._program.retire(queue);
}
const std::byte* canonical_data(const value& source) noexcept { return value_access::data(source); }
result_storage::result_storage(const prepared_raw_segment& binding) :
    _binding(binding), _data(nullptr) {
    const auto bytes = binding.result_type().byte_layout();
    if (bytes.size != 0)
        _data =
            static_cast<std::byte*>(::operator new(bytes.size, std::align_val_t(bytes.alignment)));
}
result_storage::~result_storage() {
    if (_initialized) _binding.destroy_result(_data);
    if (_data)
        ::operator delete(_data, std::align_val_t(_binding.result_type().byte_layout().alignment));
}
} // namespace cel::detail
namespace cel {
using detail::align_offset;
using detail::borrowed_value;
using detail::frame;
using detail::prepared_raw_segment;
using detail::raw_segment;

stack_reservation stack_requirements::for_prefix(std::size_t cursor, std::size_t depth) const {
    return {{detail::checked_add(align_offset(cursor, alignment), frame_extent), alignment},
            detail::checked_add(depth, live_slots)};
}
detail::construction_effect dyn_segment::check_operation(std::span<const value_type> inputs,
                                                         std::span<const value_type> outputs) {
    assert(outputs.size() <= 1);
    if (inputs.size() > _stack.size()) throw std::invalid_argument("operation operand underflow");
    // Compare the source-order suffix before emitting any effects.
    for (std::size_t i = 0; i != inputs.size(); ++i) {
        detail::require_ownership(inputs[i]);
        detail::require_compatible(inputs[i], _stack[_stack.size() - inputs.size() + i]);
    }
    std::ranges::for_each(outputs, detail::require_ownership);
    const auto remaining = _stack.size() - inputs.size();
    const auto depth = detail::checked_add(remaining, outputs.size());
    (void)detail::checked_add(_origin.depth, depth);
    detail::construction_effect effect{origin_below(inputs.size()).bytes, _requirements};
    // Account for the canonical result only after the consumed suffix has retired.
    for (const auto& output : outputs) {
        const auto bytes = output.byte_layout();
        effect.cursor =
            detail::checked_add(align_offset(effect.cursor, bytes.alignment), bytes.size);
        effect.requirements.alignment = std::max(effect.requirements.alignment, bytes.alignment);
    }
    effect.requirements.frame_extent =
        std::max(effect.requirements.frame_extent, effect.cursor - _origin.bytes);
    effect.requirements.live_slots = std::max(effect.requirements.live_slots, depth);
    _stack.reserve(depth);
    _end_cursors.reserve(depth);
    _known_calls.reserve(depth);
    return effect;
}
// NOLINTNEXTLINE(bugprone-exception-escape): pre-reserved, nonthrowing copies.
void dyn_segment::apply_operation(std::size_t consumed,
                                  std::span<const value_type> outputs,
                                  const detail::construction_effect& effect) noexcept {
    assert(consumed <= _stack.size());
    assert(outputs.size() <= 1);
    assert(_stack.size() == _end_cursors.size());
    assert(_stack.capacity() >= _stack.size() - consumed + outputs.size());
    assert(_end_cursors.capacity() >= _stack.size() - consumed + outputs.size());
    // Remove consumed witnesses and publish only pre-reserved output metadata.
    for (std::size_t i = 0; i != consumed; ++i) {
        _stack.pop_back();
        _end_cursors.pop_back();
        _known_calls.pop_back();
    }
    for (const auto& output : outputs) {
        _stack.push_back(output);
        _end_cursors.push_back(effect.cursor);
        _known_calls.push_back(std::nullopt);
    }
    _requirements = effect.requirements;
}
void dyn_segment::push_argument(std::size_t index, value_type type) {
    detail::require_ownership(type);
    const auto existing = std::ranges::find(_reads, index, &detail::argument_read::index);
    if (existing != _reads.end()) detail::require_compatible(existing->type, type);
    const bool added = existing == _reads.end();
    const std::array outputs{type};
    const auto effect = check_operation({}, outputs);
    if (added) _reads.reserve(_reads.size() + 1);
    detail::segment_access::append(_body, [index, type](frame& scope) {
        const auto input = scope.argument(index);
        detail::frame_access::push(
            scope, type, input.data,
            [](const void* source, const value_type& witness, std::byte* destination) {
                detail::clone_payload(witness, static_cast<const std::byte*>(source), destination);
            });
    });
    if (added) _reads.push_back({index, type});
    apply_operation(0, outputs, effect);
}
void dyn_segment::just(value literal) {
    const auto type = literal.type();
    const std::array outputs{type};
    const auto effect = check_operation({}, outputs);
    detail::segment_access::append(_body, detail::literal_instruction{std::move(literal)});
    apply_operation(0, outputs, effect);
}
stack_origin dyn_segment::snapshot() const { return origin_below(0); }
stack_origin dyn_segment::origin_below(std::size_t count) const {
    if (count > _stack.size()) throw std::invalid_argument("operation operand underflow");
    const auto remaining = _stack.size() - count;
    return {remaining == 0 ? _origin.bytes : _end_cursors[remaining - 1],
            detail::checked_add(_origin.depth, remaining)};
}
stack_origin dyn_segment::before_top() const { return origin_below(1); }
void dyn_segment::pack(value_type type,
                       std::span<const std::size_t> permutation,
                       stack_origin ambient) {
    const auto count = type.offsets().size();
    if (count > _stack.size()) throw std::invalid_argument("packing operand underflow");
    const auto expected = origin_below(count);
    if (ambient != expected) throw std::invalid_argument("wrong packing origin");
    if (permutation.size() != count) throw std::invalid_argument("wrong packing permutation");
    std::vector<bool> seen(count, false);
    std::vector<value_type> inputs;
    inputs.reserve(count);
    for (auto index : permutation) {
        if (index >= count || seen[index]) throw std::invalid_argument("invalid permutation");
        seen[index] = true;
        inputs.push_back(type.kind() == type_kind::range ? type.element() : type.fields()[index]);
    }
    const std::array outputs{type};
    const auto effect = check_operation(inputs, outputs);
    auto stage_type = value_type::tuple(inputs);
    std::vector<std::size_t> mapping(permutation.begin(), permutation.end());
    detail::segment_access::append(
        _body, [type, stage_type, mapping = std::move(mapping)](frame& scope) {
            detail::canonical_owner stage(stage_type), result(type);
            const auto fields = stage_type.fields();
            detail::frame_access::reserve_result(scope, type, fields.size());
            // All outer allocations precede the first source ownership transfer.
            for (std::size_t i = fields.size(); i != 0; --i) {
                detail::frame_access::release_result(
                    scope, fields[i - 1],
                    detail::canonical_at(stage.bytes.data(), stage_type.offsets()[i - 1]));
                ++stage.suffix;
            }
            for (std::size_t i = 0; i != fields.size(); ++i)
                detail::transfer_bytes(
                    detail::canonical_at(result.bytes.data(), type.offsets()[mapping[i]]),
                    detail::canonical_at(stage.bytes.data(), stage_type.offsets()[i]),
                    fields[i].byte_layout().size);
            stage.suffix = 0;
            result.live = true;
            detail::frame_access::push(scope, type, &result, detail::write_owner);
        });
    apply_operation(count, outputs, effect);
}
void dyn_segment::make_tuple(std::size_t count, stack_origin ambient) {
    if (count > _stack.size()) throw std::invalid_argument("tuple operand underflow");
    const auto fields = std::span(_stack).last(count);
    std::vector<std::size_t> mapping(count);
    for (std::size_t i = 0; i != count; ++i)
        mapping[i] = i;
    pack(value_type::tuple(fields), mapping, ambient);
}
void dyn_segment::make_record(value_type record, stack_origin ambient) {
    if (record.kind() != type_kind::record) throw std::invalid_argument("not a record");
    std::vector<std::size_t> mapping(record.fields().size());
    for (std::size_t i = 0; i != mapping.size(); ++i)
        mapping[i] = i;
    pack(std::move(record), mapping, ambient);
}
void dyn_segment::make_record(value_type record,
                              std::span<const std::size_t> permutation,
                              stack_origin ambient) {
    if (record.kind() != type_kind::record) throw std::invalid_argument("not a record");
    pack(std::move(record), permutation, ambient);
}
void dyn_segment::make_array(std::size_t count, value_type element, stack_origin ambient) {
    if (count > _stack.size()) throw std::invalid_argument("array operand underflow");
    const auto expected = origin_below(count);
    if (ambient != expected) throw std::invalid_argument("wrong array origin");
    detail::require_ownership(element);
    const auto type = value_type::array(element);
    const std::vector<value_type> inputs(count, element);
    const std::array outputs{type};
    const auto effect = check_operation(inputs, outputs);
    detail::segment_access::append(_body, [type, element, count](frame& scope) {
        detail::canonical_owner result(type);
        detail::array_builder packed(element, count);
        detail::frame_access::reserve_result(scope, type, count);
        // Stack operands transfer top-first into their original packed source positions.
        for (std::size_t i = 0; i != count; ++i) {
            detail::frame_access::release_result(scope, element, packed.destination());
            packed.initialized();
        }
        packed.release_to(result.bytes.data());
        result.live = true;
        detail::frame_access::push(scope, type, &result, detail::write_owner);
    });
    apply_operation(count, outputs, effect);
}
void dyn_segment::project(std::size_t index, type_kind category) {
    if (_stack.empty() || _stack.back().kind() != category)
        throw std::invalid_argument("wrong projection operand");
    const auto type = _stack.back();
    if (index >= type.fields().size())
        throw std::invalid_argument("projection index out of bounds");
    const std::array inputs{type}, outputs{type.fields()[index]};
    const auto effect = check_operation(inputs, outputs);
    detail::segment_access::append(_body, [type, result_type = outputs[0], index](frame& scope) {
        detail::canonical_owner source(type), result(result_type);
        detail::frame_access::reserve_result(scope, result_type, 1);
        detail::frame_access::release_result(scope, type, source.bytes.data());
        source.live = true;
        detail::value_access::project(type, source.bytes.data(), index, result.bytes.data());
        source.live = false;
        result.live = true;
        detail::frame_access::push(scope, result_type, &result, detail::write_owner);
    });
    apply_operation(1, outputs, effect);
}
void dyn_segment::project_tuple(std::size_t index) { project(index, type_kind::tuple); }
void dyn_segment::project_record(std::string_view name) {
    if (_stack.empty() || _stack.back().kind() != type_kind::record)
        throw std::invalid_argument("not a record operand");
    const auto names = _stack.back().field_names();
    const auto found = std::ranges::find(names, name);
    if (found == names.end()) throw std::invalid_argument("record field does not exist");
    project(static_cast<std::size_t>(found - names.begin()), type_kind::record);
}
void dyn_segment::index_array() {
    if (_stack.size() < 2 || _stack[_stack.size() - 2].kind() != type_kind::array)
        throw std::invalid_argument("not an array operand");
    const auto type = _stack[_stack.size() - 2];
    const auto index_type = value_type::leaf<double>();
    const std::array inputs{type, index_type};
    const std::array outputs{type.element()};
    const auto effect = check_operation(inputs, outputs);
    detail::segment_access::append(_body, [type, index_type](frame& scope) {
        const auto index = detail::frame_access::acquire(scope, [&](auto view) {
            double number = 0;
            std::memcpy(&number, view.operand(0, index_type), sizeof(number));
            // The exclusive upper bound 2^digits cannot round SIZE_MAX upward into admission.
            const auto upper = std::ldexp(1.0, std::numeric_limits<std::size_t>::digits);
            if (!std::isfinite(number) || number < 0 || std::trunc(number) != number ||
                number >= upper)
                throw std::out_of_range("invalid floating array index");
            // The exclusive bound proves this cast representable; compare the exact integer
            // length afterwards, without rounding array length to f64, before child access.
            const auto converted = static_cast<std::size_t>(number);
            if (converted >= detail::value_access::array_size(view.operand(1, type)))
                throw std::out_of_range("array index out of bounds");
            return converted;
        });
        detail::canonical_owner source(type), result(type.element());
        detail::frame_access::reserve_result(scope, type.element(), 2);
        detail::frame_access::discard(scope, 1);
        detail::frame_access::release_result(scope, type, source.bytes.data());
        source.live = true;
        detail::value_access::project(type, source.bytes.data(), index, result.bytes.data());
        source.live = false;
        result.live = true;
        detail::frame_access::push(scope, result.type, &result, detail::write_owner);
    });
    apply_operation(2, outputs, effect);
}
void dyn_segment::make_range(range_kind kind) {
    auto type = value_type::range(kind, value_type::leaf<double>());
    const auto count = type.offsets().size();
    if (count > _stack.size()) throw std::invalid_argument("range operand underflow");
    const auto ambient = origin_below(count);
    std::vector<std::size_t> mapping(count);
    for (std::size_t i = 0; i != count; ++i)
        mapping[i] = i;
    pack(std::move(type), mapping, ambient);
}
void dyn_segment::if_else(dyn_segment then_body, dyn_segment else_body) {
    const auto ambient = before_top();
    if (then_body._origin != ambient || else_body._origin != ambient)
        throw std::invalid_argument("wrong branch origin");
    if (then_body._stack.size() != 1 || else_body._stack.size() != 1)
        throw std::invalid_argument("branch must have one result");
    detail::require_compatible(then_body._stack.back(), else_body._stack.back());
    const std::array inputs{value_type::leaf<bool>()}, outputs{then_body._stack.back()};
    auto effect = check_operation(inputs, outputs);
    auto reads = _reads;
    // Merge both branches' declarations before publishing either instruction.
    for (const auto* body : {&then_body, &else_body}) {
        for (const auto& read : body->_reads) {
            const auto found = std::ranges::find(reads, read.index, &detail::argument_read::index);
            if (found != reads.end())
                detail::require_compatible(found->type, read.type);
            else
                reads.push_back(read);
        }
        effect.requirements.alignment =
            std::max(effect.requirements.alignment, body->_requirements.alignment);
        const auto extent = detail::checked_add(ambient.bytes, body->_requirements.frame_extent);
        effect.requirements.frame_extent =
            std::max(effect.requirements.frame_extent, extent - _origin.bytes);
        const auto slots = detail::checked_add(ambient.depth, body->_requirements.live_slots);
        effect.requirements.live_slots =
            std::max(effect.requirements.live_slots, slots - _origin.depth);
    }
    auto yes = std::move(then_body).finish();
    auto no = std::move(else_body).finish();
    detail::segment_access::append(
        _body, detail::branch_instruction{std::move(yes._program), std::move(no._program)});
    _reads.swap(reads);
    apply_operation(1, outputs, effect);
}
void dyn_segment::logical_and(dyn_segment rhs) {
    if (rhs._stack.size() != 1) throw std::invalid_argument("logical rhs must have one result");
    detail::require_compatible(value_type::leaf<bool>(), rhs._stack.back());
    dyn_segment no(before_top());
    no.just(value::from(false));
    if_else(std::move(rhs), std::move(no));
}
void dyn_segment::logical_or(dyn_segment rhs) {
    if (rhs._stack.size() != 1) throw std::invalid_argument("logical rhs must have one result");
    detail::require_compatible(value_type::leaf<bool>(), rhs._stack.back());
    dyn_segment yes(before_top());
    yes.just(value::from(true));
    if_else(std::move(yes), std::move(rhs));
}
void dyn_segment::push_closure(compiled_segment body, std::span<const value_type> parameters) {
    callable fn(std::move(body), parameters);
    const auto required = fn.requirements();
    just(fn.to_value());
    _known_calls.back() = required;
}
void dyn_segment::invoke_closure(std::size_t arity) {
    if (arity >= _stack.size()) throw std::invalid_argument("callable operand underflow");
    const auto position = _stack.size() - arity - 1;
    const auto type = _stack[position];
    if (!detail::callable_access::installed(type) || type.parameters().size() != arity)
        throw std::invalid_argument("wrong callable signature");
    std::vector<value_type> inputs{type};
    inputs.insert(inputs.end(), type.parameters().begin(), type.parameters().end());
    const std::array outputs{type.result()};
    auto effect = check_operation(inputs, outputs);
    const auto& known_call = _known_calls[position];
    if (known_call.has_value()) {
        const auto ambient = origin_below(arity + 1);
        const auto required = known_call->for_prefix(ambient.bytes, ambient.depth);
        effect.requirements.alignment =
            std::max(effect.requirements.alignment, required.bytes.alignment);
        effect.requirements.frame_extent =
            std::max(effect.requirements.frame_extent, required.bytes.size - _origin.bytes);
        effect.requirements.live_slots =
            std::max(effect.requirements.live_slots, required.slots - _origin.depth);
    }
    auto stage_type = value_type::tuple(inputs);
    detail::segment_access::append(_body, [type, stage_type](frame& scope) {
        auto code = detail::frame_access::acquire(scope, [&](auto view) {
            return detail::callable_access::read(view.operand(type.parameters().size(), type));
        });
        detail::callable_invocation entry(std::move(code));
        detail::canonical_owner stage(stage_type), result(type.result());
        std::vector<borrowed_value> views;
        views.reserve(type.parameters().size());
        for (std::size_t i = 0; i != type.parameters().size(); ++i)
            views.push_back(
                {type.parameters()[i],
                 detail::canonical_at(stage.bytes.data(), stage_type.offsets()[i + 1])});
        const auto fields = stage_type.fields();
        detail::frame_access::reserve_result(scope, type.result(), fields.size());
        // Staging/table/result allocations precede the first ownership transfer.
        for (std::size_t i = fields.size(); i != 0; --i) {
            detail::frame_access::release_result(
                scope, fields[i - 1],
                detail::canonical_at(stage.bytes.data(), stage_type.offsets()[i - 1]));
            ++stage.suffix;
        }
        entry.call_into(detail::frame_access::stack(scope), views, result.bytes.data());
        result.live = true;
        detail::frame_access::push(scope, result.type, &result, detail::write_owner);
    });
    apply_operation(inputs.size(), outputs, effect);
}
compiled_segment dyn_segment::finish() && {
    if (_stack.size() > 1) throw std::invalid_argument("final stack has more than one result");
    if (_stack.empty()) just(value{});
    auto program = detail::retirement_ptr<const detail::frozen_program>(new detail::frozen_program(
        std::move(_body), std::move(_reads), _stack.back(), _requirements, _origin));
    _stack.clear();
    _end_cursors.clear();
    _known_calls.clear();
    _requirements = {};
    return compiled_segment(std::move(program));
}
compiled_segment::compiled_segment(
    detail::retirement_ptr<const detail::frozen_program> program) noexcept :
    _program(std::move(program)) {}
const value_type& compiled_segment::result_type() const noexcept {
    assert(_program);
    return _program->result;
}
const stack_requirements& compiled_segment::requirements() const noexcept {
    assert(_program);
    return _program->requirements;
}
value compiled_segment::execute(raw_stack& stack, std::span<const value> arguments) const {
    if (!_program) throw std::logic_error("moved-from compiled program");
    if (_program->origin != stack_origin{}) throw std::invalid_argument("dependent program");
    // Validate every emitted read before entering the executable body.
    for (const auto& read : _program->reads) {
        if (read.index >= arguments.size()) throw std::invalid_argument("missing argument read");
        detail::require_compatible(read.type, arguments[read.index].type());
    }
    frame scope(stack, arguments);
    _program->body.execute(scope);
    return scope.pop_value();
}
prepared_raw_segment compiled_segment::prepare_raw(std::span<const value_type> arguments,
                                                   const value_type& result) const {
    if (!_program) throw std::logic_error("moved-from compiled program");
    if (_program->origin != stack_origin{}) throw std::invalid_argument("dependent program");
    detail::require_ownership(result);
    detail::require_compatible(_program->result, result);
    // Only emitted reads need clone capability; unused inputs are never materialized.
    for (const auto& read : _program->reads) {
        if (read.index >= arguments.size()) throw std::invalid_argument("undeclared argument read");
        detail::require_compatible(read.type, arguments[read.index]);
        detail::require_ownership(arguments[read.index]);
    }
    return prepared_raw_segment(_program, arguments, result);
}
prepared_raw_segment detail::segment_access::prepare_raw(const compiled_segment& program,
                                                         std::span<const value_type> arguments,
                                                         const value_type& result) {
    return program.prepare_raw(arguments, result);
}
prepared_raw_segment::prepared_raw_segment(
    detail::retirement_ptr<const detail::frozen_program> program,
    std::span<const value_type> arguments,
    value_type result) :
    _program(std::move(program)), _arguments(arguments.begin(), arguments.end()),
    _result(std::move(result)), _used(arguments.size(), false) {
    // Freeze which input positions need materialization in typed convenience calls.
    for (const auto& read : _program->reads)
        _used[read.index] = true;
}
const stack_requirements& prepared_raw_segment::requirements() const noexcept {
    assert(_program);
    return _program->requirements;
}
void prepared_raw_segment::call_into(raw_stack& stack,
                                     std::span<const borrowed_value> arguments,
                                     std::byte* result_destination) const {
    if (!_program) throw std::logic_error("moved-from prepared program");
    if (arguments.size() != _arguments.size()) throw std::invalid_argument("wrong prepared arity");
    frame scope(stack, arguments);
    _program->body.execute(scope);
    detail::frame_access::release_result(scope, _result, result_destination);
}
void prepared_raw_segment::destroy_result(std::byte* result) const noexcept {
    assert(_program);
    detail::destroy_payload(_result, result);
}
} // namespace cel

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include <doctest/doctest.h>

namespace {
/// Tracks opaque result owners to detect sibling cleanup and accidental projection cloning.
int opaque_owners = 0;
/// Counts canonical opaque clones, independently of outer-storage allocation.
int opaque_clones = 0;
/// Identifies one live opaque owner in canonical bytes.
struct opaque_carrier {
    int* owner; ///< Owns one test resource.
};
/// Clones a test resource into an independent canonical carrier.
void clone_opaque(const std::byte* source, std::byte* destination) {
    opaque_carrier input{};
    std::memcpy(&input, source, sizeof(input));
    auto owner = std::make_unique<int>(*input.owner);
    const opaque_carrier output{owner.release()};
    std::memcpy(destination, &output, sizeof(output));
    ++opaque_owners;
    ++opaque_clones;
}
/// Destroys one live canonical resource.
void destroy_opaque(std::byte* source) noexcept {
    opaque_carrier input{};
    std::memcpy(&input, source, sizeof(input));
    delete input.owner;
    --opaque_owners;
}
/// Builds a trusted opaque owner with the specified installed complete witness.
cel::value make_opaque(const cel::value_type& type) {
    auto owner = std::make_unique<int>(4);
    const opaque_carrier input{owner.get()};
    auto result = cel::detail::value_access::take(type, reinterpret_cast<const std::byte*>(&input));
    [[maybe_unused]] auto* retired_source = owner.release();
    ++opaque_owners;
    return result;
}
TEST_CASE("consuming projection transfers selected owners and retires siblings without cloning") {
    const auto signature = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<bool>()),
        {clone_opaque, destroy_opaque});
    {
        cel::dyn_segment b;
        b.just(make_opaque(signature));
        b.just(make_opaque(signature));
        b.make_array(2, signature, {});
        b.make_tuple(1, {});
        b.project_tuple(0);
        b.just(cel::value::from(1.0));
        b.index_array();
        auto program = std::move(b).finish();
        cel::raw_stack stack;
        const auto clones = opaque_clones;
        {
            auto result = program.execute(stack, {});
            CHECK(result.type().same_shape(signature));
            CHECK(opaque_clones - clones == 2);
            CHECK(opaque_owners == 3);
        }
        CHECK(opaque_owners == 2);
        CHECK(stack.depth() == 0);
    }
    CHECK(opaque_owners == 0);
}
} // namespace

#endif

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include <array>
#include <doctest/doctest.h>
#include <limits>
#include <memory>
#include <tuple>
#include <vector>

namespace {
/// Counts live synthetic callable-carrier owners without implementing callable bodies.
int live_carriers = 0;
/// Clones an independently owned integer through the foundation's opaque ownership contract.
void clone_carrier(const std::byte* source, std::byte* destination) {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), source, sizeof(pointer));
    auto owner = std::make_unique<int>(*pointer);
    pointer = owner.release();
    std::memcpy(destination, static_cast<const void*>(&pointer), sizeof(pointer));
    ++live_carriers;
}
/// Retires one initialized synthetic carrier.
void destroy_carrier(std::byte* bytes) noexcept {
    int* pointer = nullptr;
    std::memcpy(static_cast<void*>(&pointer), bytes, sizeof(pointer));
    delete pointer;
    --live_carriers;
}
/// Supplies an incompatible ownership pair with the same semantic signature.
void destroy_other_carrier(std::byte* bytes) noexcept { destroy_carrier(bytes); }

/// Exercises root admission independently of callable-parameter restrictions.
struct alignas(256) client_leaf {
    int number; ///< Retains a client-defined scalar.
};
/// Holds an owned callable field for typed-result lifetime tests.
struct segment_callable_record {
    /// Owns one callable value.
    cel::value code;
};

/// Holds test input for ownership-mismatch adapter regression checks.
struct ownership_gap_payload {
    /// Seeds the synthetic callable carrier.
    int seed;
};

/// Returns the callable descriptor with the primary ownership pair.
const cel::value_type& primary_callable_type() {
    static const auto descriptor = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<double>()),
        {clone_carrier, destroy_carrier});
    return descriptor;
}

/// Returns the callable descriptor with an incompatible ownership pair.
const cel::value_type& foreign_callable_type() {
    static const auto descriptor = cel::detail::type_access::install_callable_ownership(
        cel::value_type::callable({}, cel::value_type::leaf<double>()),
        {clone_carrier, destroy_other_carrier});
    return descriptor;
}

/// Returns the declared nominal record descriptor with the primary callable ownership pair.
const cel::value_type& declared_ownership_gap_type() {
    static const auto descriptor = cel::value_type::record(
        "tests.ownership_gap", std::array{cel::record_field{"code", primary_callable_type()}});
    return descriptor;
}

/// Returns a same-shape nominal record descriptor with incompatible callable ownership.
const cel::value_type& foreign_ownership_gap_type() {
    static const auto descriptor = cel::value_type::record(
        "tests.ownership_gap", std::array{cel::record_field{"code", foreign_callable_type()}});
    return descriptor;
}

/// Returns one callable value with synthetic owned carrier storage.
cel::value make_foreign_callable_value(int seed) {
    int payload = seed;
    int* pointer = &payload;
    std::array<std::byte, sizeof(pointer)> carrier{};
    std::memcpy(carrier.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    return cel::detail::value_access::copy(foreign_callable_type(), carrier.data());
}

/// Returns one callable value with the primary ownership pair.
cel::value make_primary_callable_value(int seed) {
    int payload = seed;
    int* pointer = &payload;
    std::array<std::byte, sizeof(pointer)> carrier{};
    std::memcpy(carrier.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    return cel::detail::value_access::copy(primary_callable_type(), carrier.data());
}
/// Supplies named overloads that require an exact explicit signature selection.
struct overloaded_callback {
    /// Doubles one exact operand.
    double operator()(double number) const { return number * 2.0; }
    /// Adds two exact operands.
    double operator()(double a, double b) const { return a + b; }
};

/// Exercises invocation through the required const lvalue callback boundary.
struct lvalue_callback {
    /// Increments its exact operand through a const lvalue.
    double operator()(double number) const& { return number + 1.0; }
};

/// Exercises nonthrowing invocation through the same const lvalue boundary.
struct lvalue_noexcept_callback {
    /// Increments its exact operand without throwing.
    double operator()(double number) const& noexcept { return number + 2.0; }
};

/// Supplies an introspection-ineligible invocation through an exact function-pointer conversion.
struct pointer_callback {
    using function = double (*)(double); ///< Describes its explicit concrete invocation.

    /// Doubles one exact operand.
    static double twice(double number) { return number * 2.0; }
    /// Returns its exact function pointer.
    operator function() const noexcept { return twice; }
};

/// Detects exact explicit member selection without hard introspection errors.
template <class Signature, class F>
concept signature_selectable = requires(F callback) {
    cel::with_signature<Signature>(std::move(callback));
};
} // namespace
namespace cel {
template <>
struct record_adapter<::segment_callable_record> {
    /// Returns the callable record schema.
    static value_type type() {
        static const auto descriptor =
            value_type::record("tests.segment_callable_record",
                               std::array{record_field{"code", primary_callable_type()}});
        return descriptor;
    }
    /// Encodes the callable field as an owned canonical value.
    static value to_value(const ::segment_callable_record& source) {
        return value::record(type(), std::array{source.code});
    }
    /// Copies the owned callable field from a validated record.
    static ::segment_callable_record from_value(const value& source) {
        if (!source.type().same_shape(type()))
            throw std::invalid_argument("wrong callable record type");
        return {.code = source.field("code")};
    }
};

template <>
struct record_adapter<ownership_gap_payload> {
    /// Returns the declared nominal descriptor with the primary ownership pair.
    static value_type type() { return declared_ownership_gap_type(); }
    /// Encodes the payload as a same-shape record with incompatible callable ownership.
    static value to_value(const ownership_gap_payload& source) {
        auto code = make_foreign_callable_value(source.seed);
        return value::record(foreign_ownership_gap_type(), std::array{code});
    }
    /// Decodes this payload from a validated record value.
    static ownership_gap_payload from_value(const value& source) {
        if (!source.type().same_shape(type()))
            throw std::invalid_argument("wrong ownership-gap type");
        return {.seed = 0};
    }
};
} // namespace cel
namespace {
TEST_CASE("recursive preparation mismatch preserves nominal and leaf identity") {
    using input = std::tuple<std::vector<double>, bool>;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::detail::concrete_adapter<input>::type());
    auto program = std::move(builder).finish();
    CHECK_THROWS_AS((program.prepare<input, std::tuple<std::vector<bool>, bool>>()),
                    std::invalid_argument);
    CHECK_THROWS_AS((program.prepare<std::tuple<std::vector<double>, double>, input>()),
                    std::invalid_argument);
    cel::raw_stack stack;
    const std::array wrong{cel::value::from(std::tuple{std::vector{true}, true})};
    CHECK_THROWS_AS(program.execute(stack, wrong), std::invalid_argument);
}
TEST_CASE("typed composite calls borrow inputs and return disjoint nested results") {
    using input = std::tuple<std::vector<std::vector<double>>, bool>;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::detail::concrete_adapter<input>::type());
    auto prepared = std::move(builder).finish().prepare<input, input>();
    cel::raw_stack stack;
    input source{{{1.0, 2.0}}, true};
    auto first = prepared.call(stack, source);
    auto second = prepared.call(stack, source);
    std::get<0>(first)[0][0] = 7.0;
    CHECK(std::get<0>(source)[0][0] == 1.0);
    CHECK(std::get<0>(second)[0][0] == 1.0);
    std::get<0>(source)[0][1] = 9.0;
    CHECK(std::get<0>(second)[0][1] == 2.0);
}
TEST_CASE("typed zero-sized unit arrays retain nested logical lengths") {
    using units = std::vector<std::vector<std::tuple<>>>;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::detail::concrete_adapter<units>::type());
    auto prepared = std::move(builder).finish().prepare<units, units>();
    const units input{std::vector<std::tuple<>>(3), {}, std::vector<std::tuple<>>(1)};
    cel::raw_stack stack;
    auto result = prepared.call(stack, input);
    REQUIRE(result.size() == 3);
    CHECK(result[0].size() == 3);
    CHECK(result[1].empty());
    CHECK(result[2].size() == 1);
    result[0].clear();
    CHECK(input[0].size() == 3);
    CHECK(prepared.call(stack, input)[0].size() == 3);
}
TEST_CASE("runtime-bound owning output has a public retirement boundary") {
    cel::dyn_segment builder;
    builder.op0<std::vector<std::vector<double>>>(
        [] { return std::vector<std::vector<double>>{{2.0}, {3.0}}; });
    auto program = std::move(builder).finish();
    auto prepared = cel::detail::segment_access::prepare_raw(program, {}, program.result_type());
    const auto layout = prepared.result_type().byte_layout();
    auto* destination =
        static_cast<std::byte*>(::operator new(layout.size, std::align_val_t(layout.alignment)));
    cel::raw_stack stack;
    prepared.call_into(stack, {}, destination);
    auto first = cel::detail::borrowed_value{prepared.result_type(), destination}
                     .as<std::vector<std::vector<double>>>();
    prepared.destroy_result(destination);
    first[0][0] = 8.0;
    prepared.call_into(stack, {}, destination);
    CHECK(cel::detail::borrowed_value{prepared.result_type(), destination}
              .as<std::vector<std::vector<double>>>()[0][0] == 2.0);
    prepared.destroy_result(destination);
    ::operator delete(destination, std::align_val_t(layout.alignment));
    CHECK(stack.depth() == 0);
}

TEST_CASE("runtime-bound exact record identity and shared deep metadata") {
    const std::array fields{cel::record_field{"flag", cel::value_type::leaf<bool>()}};
    auto type = cel::value_type::record("named", fields);
    cel::dyn_segment builder;
    builder.push_argument(0, type);
    auto program = std::move(builder).finish();
    const std::array wrong{cel::value_type::record("other", fields)};
    CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(program, wrong, type),
                    std::invalid_argument);
    const std::array arguments{type};
    auto prepared = cel::detail::segment_access::prepare_raw(program, arguments, type);
    const std::array contents{cel::value::from(true)};
    auto source = cel::value::record(type, contents);
    const std::array views{
        cel::detail::borrowed_value{type, cel::detail::value_access::data(source)}};
    alignas(bool) std::array<std::byte, sizeof(bool)> result{};
    cel::raw_stack stack;
    prepared.call_into(stack, views, result.data());
    CHECK(cel::detail::value_access::copy(type, result.data()).field("flag").as<bool>());
    prepared.destroy_result(result.data());
    auto deep = cel::value_type::leaf<bool>();
    auto same = cel::value_type::leaf<bool>();
    for (int i = 0; i != 32; ++i) {
        const std::array left{deep, deep};
        const std::array right{same, same};
        deep = cel::value_type::tuple(left);
        same = cel::value_type::tuple(right);
    }
    cel::dyn_segment shared;
    shared.push_argument(0, deep);
    auto shared_program = std::move(shared).finish();
    const std::array shared_arguments{same};
    CHECK_NOTHROW(cel::detail::segment_access::prepare_raw(shared_program, shared_arguments, same));
}

TEST_CASE("throwing callbacks preserve caller prefix and reusable raw destination") {
    bool fail = true;
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.op1<double, double>([&fail](double n) {
        if (fail) throw std::runtime_error("callback");
        return n + 1.0;
    });
    auto program = std::move(builder).finish();
    const std::array types{cel::value_type::leaf<double>()};
    auto prepared = cel::detail::segment_access::prepare_raw(program, types, types[0]);
    cel::raw_stack stack;
    stack.push(std::vector{7.0});
    const auto cursor = stack.cursor();
    double input = 2.0;
    const std::array views{
        cel::detail::borrowed_value{types[0], reinterpret_cast<const std::byte*>(&input)}};
    alignas(double) std::array<std::byte, sizeof(double)> result{};
    result.fill(std::byte{0x5a});
    CHECK_THROWS_AS(prepared.call_into(stack, views, result.data()), std::runtime_error);
    CHECK(result[0] == std::byte{0x5a});
    CHECK(stack.depth() == 1);
    CHECK(stack.cursor() == cursor);
    fail = false;
    prepared.call_into(stack, views, result.data());
    CHECK(cel::detail::borrowed_value{types[0], result.data()}.as<double>() == 3.0);
    prepared.destroy_result(result.data());
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{7.0});
}
TEST_CASE("typed record results retire their canonical owned fields") {
    auto code = make_primary_callable_value(17);
    cel::dyn_segment builder;
    builder.op0<segment_callable_record>(
        [code = std::move(code)] { return segment_callable_record{.code = code}; });
    auto prepared = std::move(builder).finish().prepare<segment_callable_record>();
    cel::raw_stack stack;
    const auto live = live_carriers;
    {
        const auto result = prepared.call(stack);
        CHECK(result.code.type().same_shape(primary_callable_type()));
        CHECK(live_carriers == live + 1);
    }
    CHECK(live_carriers == live);
}

TEST_CASE("typed record adapter rejects same-shape incompatible ownership before transfer") {
    cel::dyn_segment builder;
    builder.op0<ownership_gap_payload>([] { return ownership_gap_payload{.seed = 7}; });
    auto program = std::move(builder).finish();
    const auto result_type = cel::native_type<ownership_gap_payload>();
    auto prepared = cel::detail::segment_access::prepare_raw(program, {}, result_type);
    const auto layout = result_type.byte_layout();
    auto* destination =
        static_cast<std::byte*>(::operator new(layout.size, std::align_val_t(layout.alignment)));
    std::fill(destination, destination + layout.size, std::byte{0x5a});
    cel::raw_stack stack;
    stack.push(std::vector{9.0});
    const auto cursor = stack.cursor();
    const auto depth = stack.depth();
    const auto live = live_carriers;
    bool threw = false;
    try {
        prepared.call_into(stack, {}, destination);
    } catch (const std::invalid_argument&) {
        threw = true;
    }
    if (!threw) prepared.destroy_result(destination);
    CHECK(threw);
    if (threw) CHECK(destination[0] == std::byte{0x5a});
    CHECK(live_carriers == live);
    CHECK(stack.depth() == depth);
    CHECK(stack.cursor() == cursor);
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{9.0});
    ::operator delete(destination, std::align_val_t(layout.alignment));
}
TEST_CASE("reserved typed and runtime-bound scalar calls reuse the stack") {
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.just(cel::value::from(3.0));
    builder.op2<double, double, double>([](double a, double b) { return a - b; });
    auto program = std::move(builder).finish();
    auto typed = program.prepare<double, double>();
    const std::array types{cel::value_type::leaf<double>()};
    auto raw = cel::detail::segment_access::prepare_raw(program, types, types[0]);
    cel::raw_stack stack;
    const auto reservation = typed.requirements().for_prefix(stack.cursor(), stack.depth());
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    CHECK(typed.call(stack, 5.0) == 2.0);
    double input = 8.0;
    const std::array views{
        cel::detail::borrowed_value{types[0], reinterpret_cast<const std::byte*>(&input)}};
    alignas(double) std::array<std::byte, sizeof(double)> bytes{};
    raw.call_into(stack, views, bytes.data());
    raw.destroy_result(bytes.data());
    double typed_total = 0.0;
    double raw_total = 0.0;
    for (int i = 0; i != 1000; ++i) {
        typed_total += typed.call(stack, input);
        raw.call_into(stack, views, bytes.data());
        raw_total += cel::detail::concrete_adapter<double>::read(types[0], bytes.data());
        raw.destroy_result(bytes.data());
    }
    CHECK(typed_total == 5000.0);
    CHECK(raw_total == 5000.0);
    CHECK(stack.depth() == 0);
}
TEST_CASE("raw calls preserve composite arguments and caller prefix") {
    int effects = 0;
    cel::dyn_segment builder;
    const auto type = cel::detail::concrete_adapter<std::vector<double>>::type();
    builder.push_argument(0, type);
    builder.push_argument(0, type);
    builder.op2<double, std::vector<double>, std::vector<double>>(
        [&effects](std::vector<double> a, std::vector<double> b) {
            ++effects;
            return a[0] + b[0];
        });
    auto program = std::move(builder).finish();
    const std::array types{type};
    auto bound =
        cel::detail::segment_access::prepare_raw(program, types, cel::value_type::leaf<double>());
    const auto owner = cel::value::from(std::vector{2.0});
    const std::array views{
        cel::detail::borrowed_value{type, cel::detail::value_access::data(owner)}};
    cel::raw_stack stack;
    stack.reserve(4096, 256);
    stack.push(std::vector{9.0});
    const auto prefix = stack.cursor();
    alignas(double) std::array<std::byte, sizeof(double)> bytes{};
    CHECK(effects == 0);
    bound.call_into(stack, views, bytes.data());
    CHECK(cel::detail::borrowed_value{bound.result_type(), bytes.data()}.as<double>() == 4.0);
    bound.destroy_result(bytes.data());
    CHECK(effects == 1);
    CHECK(owner.as<std::vector<double>>() == std::vector{2.0});
    CHECK(stack.cursor() == prefix);
    CHECK(stack.depth() == 1);
    CHECK(stack.pop<std::vector<double>>() == std::vector<double>{9.0});
}

TEST_CASE("frozen raw callable results bind complete ownership and retire independently") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<client_leaf>());
    const auto type = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_carrier});
    const auto other = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_other_carrier});
    const auto composite = cel::value_type::array(type);
    const auto incompatible = cel::value_type::array(other);
    int number = 42;
    int* pointer = &number;
    alignas(void*) std::array<std::byte, sizeof(pointer)> bytes{};
    std::memcpy(bytes.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    CHECK(live_carriers == 0);
    {
        const auto owner = cel::detail::value_access::copy(type, bytes.data());
        const std::array elements{owner};
        auto literal = cel::value::array(type, elements);
        cel::dyn_segment builder;
        builder.just(std::move(literal));
        auto program = std::move(builder).finish();
        CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(program, {}, incompatible),
                        std::invalid_argument);
        CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(program, {},
                                                                 cel::value_type::array(signature)),
                        std::invalid_argument);
        auto bound = cel::detail::segment_access::prepare_raw(program, {}, composite);
        const auto layout = composite.byte_layout();
        auto* result = static_cast<std::byte*>(
            ::operator new(layout.size, std::align_val_t(layout.alignment)));
        cel::raw_stack stack;
        const auto baseline = live_carriers;
        bound.call_into(stack, {}, result);
        CHECK(live_carriers == baseline + 1);
        bound.destroy_result(result);
        CHECK(live_carriers == baseline);
        bound.call_into(stack, {}, result);
        CHECK(live_carriers == baseline + 1);
        bound.destroy_result(result);
        CHECK(live_carriers == baseline);
        ::operator delete(result, std::align_val_t(layout.alignment));
    }
    CHECK(live_carriers == 0);
}

TEST_CASE("checked callable read pairs and range identities reject before effects") {
    const auto signature = cel::value_type::callable({}, cel::value_type::leaf<bool>());
    const auto type = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_carrier});
    const auto other = cel::detail::type_access::install_callable_ownership(
        signature, {clone_carrier, destroy_other_carrier});
    int effects = 0;
    cel::dyn_segment builder;
    builder.op0<void>([&effects] { ++effects; });
    builder.push_argument(0, type);
    CHECK_THROWS_AS(builder.push_argument(0, other), std::invalid_argument);
    auto program = std::move(builder).finish();
    const std::array wrong_types{other};
    CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(program, wrong_types, type),
                    std::invalid_argument);
    int number = 3;
    int* pointer = &number;
    alignas(void*) std::array<std::byte, sizeof(pointer)> bytes{};
    std::memcpy(bytes.data(), static_cast<const void*>(&pointer), sizeof(pointer));
    const std::array wrong_values{cel::detail::value_access::copy(other, bytes.data())};
    cel::raw_stack stack;
    CHECK_THROWS_AS(program.execute(stack, wrong_values), std::invalid_argument);
    CHECK(effects == 0);
    const auto range =
        cel::value_type::range(cel::range_kind::range_full, cel::value_type::leaf<double>());
    cel::dyn_segment ranges;
    ranges.push_argument(0, range);
    auto range_program = std::move(ranges).finish();
    const std::array wrong_range{
        cel::value_type::range(cel::range_kind::range_full, cel::value_type::leaf<bool>())};
    CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(range_program, wrong_range, range),
                    std::invalid_argument);
    const std::array right_range{range};
    auto bound = cel::detail::segment_access::prepare_raw(range_program, right_range, range);
    const std::array range_input{cel::detail::borrowed_value{range, nullptr}};
    CHECK_NOTHROW(bound.call_into(stack, range_input, nullptr));
    bound.destroy_result(nullptr);
    CHECK(stack.depth() == 0);
}

TEST_CASE("zero-byte non-unit raw arguments retain complete recursive identity") {
    using zero = std::tuple<std::tuple<>, std::tuple<>>;
    const auto type = cel::detail::concrete_adapter<zero>::type();
    cel::dyn_segment builder;
    builder.push_argument(0, type);
    auto program = std::move(builder).finish();
    CHECK_THROWS_AS((program.prepare<std::tuple<>, zero>()), std::invalid_argument);
    const std::array types{type};
    auto bound = cel::detail::segment_access::prepare_raw(program, types, type);
    const std::array inputs{cel::detail::borrowed_value{type, nullptr}};
    cel::raw_stack stack;
    CHECK_NOTHROW(bound.call_into(stack, inputs, nullptr));
    bound.destroy_result(nullptr);
    CHECK(program.prepare<zero, zero>().call(stack, zero{}) == zero{});
}
TEST_CASE("exact callback selection rejects ambiguous conversions and accepts const lvalues") {
    static_assert(!signature_selectable<double(bool), overloaded_callback>);
    static_assert(!cel::detail::exact_callback<pointer_callback, double, double>());
    static_assert(!signature_selectable<double(double), pointer_callback>);
    cel::dyn_segment builder;
    builder.just(cel::value::from(3.0));
    builder.op1<double, double>(cel::with_signature<double(double)>(overloaded_callback{}));
    builder.op1<double, double>(lvalue_callback{});
    builder.op1<double, double>(lvalue_noexcept_callback{});
    cel::raw_stack stack;
    CHECK(std::move(builder).finish().prepare<double>().call(stack) == 9.0);
    cel::dyn_segment binary;
    binary.just(cel::value::from(2.0));
    binary.just(cel::value::from(5.0));
    binary.op2<double, double, double>(
        cel::with_signature<double(double, double)>(overloaded_callback{}));
    CHECK(std::move(binary).finish().prepare<double>().call(stack) == 7.0);
    cel::dyn_segment pointer;
    pointer.just(cel::value::from(4.0));
    pointer.op1<double, double>(static_cast<pointer_callback::function>(pointer_callback{}));
    pointer.op1<double, double>(pointer_callback::twice);
    CHECK(std::move(pointer).finish().prepare<double>().call(stack) == 16.0);
    cel::dyn_segment qualified;
    qualified.just(cel::value::from(3.0));
    qualified.op1<double, double>(cel::with_signature<double(double)>(lvalue_noexcept_callback{}));
    CHECK(std::move(qualified).finish().prepare<double>().call(stack) == 5.0);
}
TEST_CASE("published reservations cover hidden intermediate alignment and caller padding") {
    cel::dyn_segment builder;
    builder.op0<double>([] { return 2.0; });
    builder.op0<client_leaf>([] { return client_leaf{7}; });
    builder.op1<double, client_leaf>(
        [](const client_leaf& source) { return double(source.number); });
    builder.op2<double, double, double>([](double a, double b) { return a + b; });
    auto program = std::move(builder).finish();
    CHECK(program.result_type().byte_layout().alignment == alignof(double));
    const auto& requirements = program.requirements();
    CHECK(requirements.alignment == alignof(client_leaf));
    CHECK(requirements.frame_extent == 512);
    CHECK(requirements.live_slots == 2);
    auto typed = program.prepare<double>();
    auto raw = cel::detail::segment_access::prepare_raw(program, {}, program.result_type());
    CHECK(&typed.requirements() == &program.requirements());
    CHECK(&raw.requirements() == &program.requirements());
    cel::raw_stack stack;
    stack.push(true);
    const auto prefix_cursor = stack.cursor();
    const auto prefix_depth = stack.depth();
    const auto reservation = typed.requirements().for_prefix(prefix_cursor, prefix_depth);
    CHECK(reservation.bytes.size == 768);
    CHECK(reservation.slots == 3);
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    alignas(double) std::array<std::byte, sizeof(double)> bytes{};
    const auto result = typed.call(stack);
    raw.call_into(stack, {}, bytes.data());
    raw.destroy_result(bytes.data());
    CHECK(result == 9.0);
    CHECK(stack.cursor() == prefix_cursor);
    CHECK(stack.depth() == prefix_depth);
    CHECK(stack.pop<bool>());
}
TEST_CASE("published reservations check prefix arithmetic and reject construction overflow") {
    cel::dyn_segment builder;
    builder.op0<double>([] { return 2.0; });
    auto program = std::move(builder).finish();
    const auto maximum = std::numeric_limits<std::size_t>::max();
    CHECK_THROWS_AS(program.requirements().for_prefix(maximum, 0), std::overflow_error);
    CHECK_THROWS_AS(program.requirements().for_prefix(0, maximum), std::overflow_error);
    auto malformed = program.requirements();
    malformed.alignment = 3;
    CHECK_THROWS_AS(malformed.for_prefix(0, 0), std::invalid_argument);
    auto huge = cel::value_type::leaf<bool>();
    for (int i = 0; i != std::numeric_limits<std::size_t>::digits - 1; ++i) {
        const std::array fields{huge, huge};
        huge = cel::value_type::tuple(fields);
    }
    cel::dyn_segment wide;
    wide.push_argument(0, huge);
    CHECK_THROWS_AS(wide.push_argument(1, huge), std::overflow_error);
    auto frozen = std::move(wide).finish();
    CHECK_NOTHROW(cel::detail::segment_access::prepare_raw(frozen, std::array{huge}, huge));
    CHECK(frozen.requirements().frame_extent == huge.byte_layout().size);
    CHECK(frozen.requirements().live_slots == 1);
}
} // namespace

#endif
