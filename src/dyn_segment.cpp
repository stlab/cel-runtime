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
/// Owns checked instructions and their complete frozen boundary requirements.
/// Shared handles have reference semantics; captures are never shallow value-copied.
struct frozen_program : retirement_node {
    raw_segment body;                 ///< Owns stable captures and const dispatch records.
    std::vector<argument_read> reads; ///< Retains each declared positional clone requirement.
    value_type result;                ///< Retains the single final owner plan.
    stack_requirements requirements;  ///< Retains statically known canonical frame peaks.
    stack_origin origin;              ///< Retains independent/dependent execution identity.
    /// Freezes successfully constructed code and metadata by normal ownership transfer.
    frozen_program(raw_segment input,
                   std::vector<argument_read> arguments,
                   value_type output,
                   stack_requirements required,
                   stack_origin ambient) :
        retirement_node(dispose_program), body(std::move(input)), reads(std::move(arguments)),
        result(std::move(output)), requirements(required), origin(ambient) {}
    /// Detaches the owned arena before deleting ordinary immutable metadata.
    static void dispose_program(retirement_node& base, retirement_queue& queue) noexcept {
        auto& program = static_cast<frozen_program&>(base);
        segment_access::retire(program.body, queue);
        delete &program;
    }
};
/// Retains a canonical literal and detaches its runtime-owned children before capture deletion.
struct literal_instruction {
    value literal; ///< Owns one complete literal, independent of execution results.
    /// Pushes an independent clone onto the caller's frame.
    /// Complexity: O(canonical payload and ownership work).
    void operator()(frame& scope) const { scope.push_clone(literal); }
    /// Retires literal ownership into the enclosing executable worklist.
    /// Complexity: O(canonical ownership paths and array nodes); no allocation.
    void retire(retirement_queue& queue) noexcept { value_access::retire(literal, queue); }
};
/// Shares both immutable branches and exposes their managed ownership edges for retirement.
struct branch_instruction {
    retirement_ptr<const frozen_program> yes; ///< Retains the selected-true body.
    retirement_ptr<const frozen_program> no;  ///< Retains the selected-false body.
    /// Executes only the body selected by the consumed boolean.
    /// Complexity: O(selected body execution work).
    void operator()(frame& scope) const {
        const bool condition = scope.pop<bool>();
        (condition ? yes->body : no->body).execute(scope);
    }
    /// Retires both body references into one local last-owner operation.
    void retire(retirement_queue& queue) noexcept {
        yes.retire(queue);
        no.retire(queue);
    }
};
void segment_access::retire(cel::compiled_segment& segment, retirement_queue& queue) noexcept {
    segment._program.retire(queue);
}
void segment_access::retire(cel::prepared_raw_segment& segment, retirement_queue& queue) noexcept {
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
    detail::construction_effect effect{remaining == 0 ? _origin.bytes : _end_cursors[remaining - 1],
                                       _requirements};
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
stack_origin dyn_segment::snapshot() const {
    return {_stack.empty() ? _origin.bytes : _end_cursors.back(),
            detail::checked_add(_origin.depth, _stack.size())};
}
stack_origin dyn_segment::before_top() const {
    if (_stack.empty()) throw std::invalid_argument("no local operand");
    const auto remaining = _stack.size() - 1;
    return {remaining == 0 ? _origin.bytes : _end_cursors[remaining - 1],
            detail::checked_add(_origin.depth, remaining)};
}
void dyn_segment::pack(value_type type,
                       std::span<const std::size_t> permutation,
                       stack_origin ambient) {
    const auto count = type.offsets().size();
    if (count > _stack.size()) throw std::invalid_argument("packing operand underflow");
    const auto remaining = _stack.size() - count;
    const stack_origin expected{remaining == 0 ? _origin.bytes : _end_cursors[remaining - 1],
                                detail::checked_add(_origin.depth, remaining)};
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
    const auto remaining = _stack.size() - count;
    const stack_origin expected{remaining == 0 ? _origin.bytes : _end_cursors[remaining - 1],
                                detail::checked_add(_origin.depth, remaining)};
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
    const auto remaining = _stack.size() - count;
    const stack_origin ambient{remaining == 0 ? _origin.bytes : _end_cursors[remaining - 1],
                               detail::checked_add(_origin.depth, remaining)};
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
        const auto ambient = position == 0 ? _origin.bytes : _end_cursors[position - 1];
        const auto required =
            known_call->for_prefix(ambient, detail::checked_add(_origin.depth, position));
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
stack_origin compiled_segment::origin() const noexcept {
    assert(_program);
    return _program->origin;
}
std::span<const detail::argument_read> compiled_segment::argument_reads() const noexcept {
    assert(_program);
    return _program->reads;
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
