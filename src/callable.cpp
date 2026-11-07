// SPDX-License-Identifier: BSL-1.0
#include "callable_access.hpp"
#include "canonical.hpp"
#include "type_ops.hpp"
#include <cassert>
#include <stdexcept>

namespace cel::detail {
/// Owns one immutable body/signature plus its exclusive recursive-entry state.
struct callable_state {
    prepared_raw_segment body; ///< Retains frozen code and full bound parameters.
    value_type signature;      ///< Retains the installed ownership representation.
    bool active = false;       ///< Rejects recursive entry into this shared carrier.
    /// Transfers the complete prevalidated binding.
    callable_state(prepared_raw_segment input, value_type type) :
        body(std::move(input)), signature(std::move(type)) {}
};
/// Carries one owned facade pointer, never a byte-relocated public callable.
struct callable_carrier {
    callable* owner; ///< Owns the reference-semantic facade.
};
static_assert(std::is_trivially_copyable_v<callable_carrier>);
static_assert(sizeof(callable_carrier) == sizeof(void*));
static_assert(alignof(callable_carrier) == alignof(void*));
const callable& callable_access::read(const std::byte* bytes) noexcept {
    callable_carrier carrier{};
    std::memcpy(&carrier, bytes, sizeof(carrier));
    return *carrier.owner;
}
/// Writes one uniquely owned facade pointer into canonical carrier bytes.
void write_callable(std::byte* destination, callable* pointer) noexcept {
    const callable_carrier carrier{pointer};
    std::memcpy(destination, &carrier, sizeof(carrier));
}
/// Clones the carrier owner while explicitly sharing reference-semantic frozen code.
void clone_callable(const std::byte* source, std::byte* destination) {
    auto owner = std::make_unique<callable>(callable_access::read(source));
    write_callable(destination, owner.release());
}
/// Retires the facade pointer carrier, releasing frozen code on its last reference.
void destroy_callable(std::byte* source) noexcept { delete &callable_access::read(source); }
bool callable_access::installed(const value_type& type) noexcept {
    if (type.kind() != type_kind::callable) return false;
    const auto ops = type_access::operations(type);
    return ops.clone == clone_callable && ops.destroy == destroy_callable;
}
/// Restores same-carrier availability on success or exception without allocation.
struct callable_entry {
    bool& active; ///< Borrows exclusive carrier state.
    /// Rejects recursive entry before body effects.
    explicit callable_entry(bool& state) : active(state) {
        if (active) throw std::logic_error("same-carrier callable reentry");
        active = true;
    }
    /// Restores the idle carrier.
    ~callable_entry() { active = false; }
    /// Disallows duplicate entry ownership.
    callable_entry(const callable_entry&) = delete;
    /// Disallows duplicate assignment.
    callable_entry& operator=(const callable_entry&) = delete;
};
void callable_access::call_into(const callable& fn,
                                raw_stack& stack,
                                std::span<const borrowed_value> arguments,
                                std::byte* destination) {
    if (!fn._state) throw std::logic_error("moved-from callable");
    callable_entry entry(fn._state->active);
    fn._state->body.call_into(stack, arguments, destination);
}
} // namespace cel::detail
namespace cel {
callable::callable(compiled_segment body, std::span<const value_type> parameters) {
    auto signature = value_type::callable(parameters, body.result_type());
    detail::require_ownership(body.result_type());
    auto binding = body.prepare_raw(parameters, body.result_type());
    signature = detail::type_access::install_callable_ownership(
        signature, {detail::clone_callable, detail::destroy_callable});
    _state = std::make_shared<detail::callable_state>(std::move(binding), std::move(signature));
}
callable::~callable() = default;
const value_type& callable::type() const noexcept {
    assert(_state);
    return _state->signature;
}
const stack_requirements& callable::requirements() const noexcept {
    assert(_state);
    return _state->body.requirements();
}
value callable::call(raw_stack& stack, std::span<const value> arguments) const {
    if (!_state) throw std::logic_error("moved-from callable");
    detail::callable_entry entry(_state->active);
    const auto parameters = type().parameters();
    if (arguments.size() != parameters.size()) throw std::invalid_argument("wrong callable arity");
    // Preflight the complete declarations, including parameters the body never reads.
    for (std::size_t i = 0; i != arguments.size(); ++i)
        detail::require_compatible(parameters[i], arguments[i].type());
    std::vector<borrowed_value> views;
    views.reserve(arguments.size());
    for (std::size_t i = 0; i != arguments.size(); ++i)
        views.push_back({parameters[i], detail::value_access::data(arguments[i])});
    detail::canonical_owner result(type().result());
    _state->body.call_into(stack, views, result.bytes.data());
    result.live = true;
    auto owner = detail::value_access::take(result.type, result.bytes.data());
    result.live = false;
    return owner;
}
value callable::to_value() const {
    if (!_state) throw std::logic_error("moved-from callable");
    detail::canonical_owner carrier(type());
    auto owner = std::make_unique<callable>(*this);
    detail::write_callable(carrier.bytes.data(), owner.release());
    carrier.live = true;
    auto result = detail::value_access::take(type(), carrier.bytes.data());
    carrier.live = false;
    return result;
}
callable callable::from_value(const value& source) {
    if (!detail::callable_access::installed(source.type()))
        throw std::invalid_argument("value is not an installed callable");
    return detail::callable_access::read(detail::value_access::data(source));
}
} // namespace cel
