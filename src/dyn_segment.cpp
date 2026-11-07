// SPDX-License-Identifier: BSL-1.0
#include "frame.hpp"
#include "instruction.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <algorithm>
#include <cassert>
#include <cel/runtime/dyn_segment.hpp>
#include <stdexcept>

namespace cel::detail {
/// Owns checked instructions and their complete frozen boundary requirements.
/// Shared handles have reference semantics; captures are never shallow value-copied.
struct frozen_program {
    raw_segment body;                 ///< Owns stable captures and const dispatch records.
    std::vector<argument_read> reads; ///< Retains each declared positional clone requirement.
    value_type result;                ///< Retains the single final owner plan.
    /// Freezes successfully constructed code and metadata by normal ownership transfer.
    frozen_program(raw_segment input, std::vector<argument_read> arguments, value_type output) :
        body(std::move(input)), reads(std::move(arguments)), result(std::move(output)) {}
};
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
void dyn_segment::check_operation(std::span<const value_type> inputs,
                                  std::span<const value_type> outputs) {
    if (inputs.size() > _stack.size()) throw std::invalid_argument("operation operand underflow");
    // Compare the source-order suffix before emitting any effects.
    for (std::size_t i = 0; i != inputs.size(); ++i) {
        detail::require_ownership(inputs[i]);
        detail::require_compatible(inputs[i], _stack[_stack.size() - inputs.size() + i]);
    }
    std::ranges::for_each(outputs, detail::require_ownership);
    _stack.reserve(_stack.size() - inputs.size() + outputs.size());
}
// NOLINTNEXTLINE(bugprone-exception-escape): pre-reserved, nonthrowing copies.
void dyn_segment::apply_operation(std::size_t consumed,
                                  std::span<const value_type> outputs) noexcept {
    assert(consumed <= _stack.size());
    assert(_stack.capacity() >= _stack.size() - consumed + outputs.size());
    // Remove consumed witnesses and publish only pre-reserved output metadata.
    for (std::size_t i = 0; i != consumed; ++i)
        _stack.pop_back();
    for (const auto& output : outputs)
        _stack.push_back(output);
}
void dyn_segment::push_argument(std::size_t index, value_type type) {
    detail::require_ownership(type);
    const auto existing = std::ranges::find(_reads, index, &detail::argument_read::index);
    if (existing != _reads.end()) detail::require_compatible(existing->type, type);
    const bool added = existing == _reads.end();
    _stack.reserve(_stack.size() + 1);
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
    _stack.push_back(std::move(type));
}
void dyn_segment::just(value literal) {
    const auto type = literal.type();
    detail::require_ownership(type);
    _stack.reserve(_stack.size() + 1);
    detail::segment_access::append(
        _body, [literal = std::move(literal)](frame& scope) { scope.push_clone(literal); });
    _stack.push_back(type);
}
compiled_segment dyn_segment::finish() && {
    if (_stack.size() > 1) throw std::invalid_argument("final stack has more than one result");
    if (_stack.empty()) just(value{});
    auto program = std::make_shared<const detail::frozen_program>(std::move(_body),
                                                                  std::move(_reads), _stack.back());
    _stack.clear();
    return compiled_segment(std::move(program));
}
compiled_segment::compiled_segment(std::shared_ptr<const detail::frozen_program> program) noexcept :
    _program(std::move(program)) {}
const value_type& compiled_segment::result_type() const noexcept {
    assert(_program);
    return _program->result;
}
std::span<const detail::argument_read> compiled_segment::argument_reads() const noexcept {
    assert(_program);
    return _program->reads;
}
value compiled_segment::execute(raw_stack& stack, std::span<const value> arguments) const {
    if (!_program) throw std::logic_error("moved-from compiled program");
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
prepared_raw_segment::prepared_raw_segment(std::shared_ptr<const detail::frozen_program> program,
                                           std::span<const value_type> arguments,
                                           value_type result) :
    _program(std::move(program)), _arguments(arguments.begin(), arguments.end()),
    _result(std::move(result)), _used(arguments.size(), false) {
    // Freeze which input positions need materialization in typed convenience calls.
    for (const auto& read : _program->reads)
        _used[read.index] = true;
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
