// SPDX-License-Identifier: BSL-1.0
#include "callable_access.hpp"
#include "canonical.hpp"
#include "instruction.hpp"
#include "type_ops.hpp"
#include <cassert>
#include <stdexcept>

namespace cel::detail {
/// A shared callable binding with an exclusive invocation guard.
struct callable_state : retirement_node {
    prepared_raw_segment body; ///< Retains frozen code and full bound parameters.
    value_type signature;      ///< Retains the installed ownership representation.
    bool active = false;       ///< Rejects recursive entry into this shared carrier.
    /// Transfers the complete prevalidated binding.
    ///
    /// Complexity: transfer of the bound argument descriptors and signature.
    callable_state(prepared_raw_segment input, value_type type) :
        retirement_node(dispose_state), body(std::move(input)), signature(std::move(type)) {}
    /// Retires this binding and its signature, deferring owned executable retirement.
    ///
    /// \pre base is the live callable_state selected by its retirement callback.
    ///
    /// Complexity: argument descriptor traversal and released signature/binding metadata.
    static void dispose_state(retirement_node& base, retirement_queue& queue) noexcept {
        auto& state = static_cast<callable_state&>(base);
        segment_access::retire(state.body, queue);
        delete &state;
    }
};
namespace {
/// A canonical owner of a reference-semantic callable handle.
struct callable_carrier {
    callable* owner; ///< Owns the reference-semantic facade.
};
static_assert(std::is_trivially_copyable_v<callable_carrier>);
static_assert(sizeof(callable_carrier) == sizeof(void*));
static_assert(alignof(callable_carrier) == alignof(void*));
} // namespace
const callable& callable_access::read(const std::byte* bytes) noexcept {
    callable_carrier carrier{};
    std::memcpy(&carrier, bytes, sizeof(carrier));
    return *carrier.owner;
}
namespace {
/// Initializes a canonical carrier with the supplied owned callable handle.
///
/// \pre destination has uninitialized carrier storage and pointer is a live uniquely owned
/// callable facade; the caller transfers its ownership to the carrier.
void write_callable(std::byte* destination, callable* pointer) noexcept {
    const callable_carrier carrier{pointer};
    std::memcpy(destination, &carrier, sizeof(carrier));
}
/// Copies an installed carrier, sharing its reference-semantic code and entry guard.
///
/// \pre source contains a live installed carrier and destination has disjoint uninitialized
/// carrier storage.
///
/// Failure leaves destination uninitialized and source unchanged.
/// Complexity: allocation of one constant-size callable facade.
void clone_callable(const std::byte* source, std::byte* destination) {
    auto owner = std::make_unique<callable>(callable_access::read(source));
    write_callable(destination, owner.release());
}
/// Retires the installed carrier without freeing its outer bytes.
///
/// \pre source contains an exclusively owned live installed carrier.
///
/// Complexity: last-owned code, capture, and binding metadata retirement.
void destroy_callable(std::byte* source) noexcept { delete &callable_access::read(source); }
} // namespace
bool callable_access::installed(const value_type& type) noexcept {
    if (type.kind() != type_kind::callable) return false;
    const auto ops = type_access::operations(type);
    return ops.clone == clone_callable && ops.destroy == destroy_callable;
}
callable_invocation::callable_invocation(callable code) : _code(std::move(code)) {
    if (!_code._state) throw std::logic_error("moved-from callable");
    if (_code._state->active) throw std::logic_error("same-carrier callable reentry");
    _code._state->active = true;
}
callable_invocation::~callable_invocation() noexcept { _code._state->active = false; }
void callable_access::retire(std::byte* bytes, retirement_queue& queue) noexcept {
    auto& owner = const_cast<callable&>(read(bytes));
    owner._state.retire(queue);
    delete &owner;
}
void callable_invocation::call_into(raw_stack& stack,
                                    std::span<const borrowed_value> arguments,
                                    std::byte* destination) const {
    _code._state->body.call_into(stack, arguments, destination);
}
} // namespace cel::detail
namespace cel {
value_type bind_runtime_ownership(const value_type& type) {
    return detail::type_access::bind_runtime_ownership(
        type, {detail::clone_callable, detail::destroy_callable});
}
callable::callable(compiled_segment body, std::span<const value_type> parameters) {
    auto signature = value_type::callable(parameters, body.result_type());
    detail::require_ownership(body.result_type());
    auto binding = detail::segment_access::prepare_raw(body, parameters, body.result_type());
    signature = detail::type_access::install_callable_ownership(
        signature, {detail::clone_callable, detail::destroy_callable});
    _state = detail::retirement_ptr<detail::callable_state>(
        new detail::callable_state(std::move(binding), std::move(signature)));
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
    detail::callable_invocation entry(*this);
    const auto parameters = type().parameters();
    if (arguments.size() != parameters.size()) throw std::invalid_argument("wrong callable arity");
    // Preflight the complete declarations, including parameters the body never reads.
    for (std::size_t i = 0; i != arguments.size(); ++i)
        detail::require_compatible(parameters[i], arguments[i].type());
    std::vector<detail::borrowed_value> views;
    views.reserve(arguments.size());
    for (std::size_t i = 0; i != arguments.size(); ++i)
        views.push_back({parameters[i], detail::value_access::data(arguments[i])});
    detail::canonical_owner result(type().result());
    entry.call_into(stack, views, result.bytes.data());
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

#include <cpp-library/testing.hpp>
#if CPP_LIBRARY_TESTING()
#include <array>
#include <cel/runtime.hpp>
#include <doctest/doctest.h>
#include <ostream>

namespace {
/// Supplies a hidden over-aligned intermediate whose final callable result is scalar.
struct alignas(256) wide_result {
    bool flag; ///< Retains the intermediate result.
};
/// Counts foreign ownership operations to detect access before ABI rejection.
int foreign_clones = 0;
/// Counts foreign canonical retirement.
int foreign_destroys = 0;
/// Copies a foreign opaque carrier without treating its bytes as installed callable code.
void clone_foreign(const std::byte* source, std::byte* destination) {
    std::memcpy(destination, source, sizeof(void*));
    ++foreign_clones;
}
/// Retires the resource-free foreign representation without dereferencing it.
void destroy_foreign(std::byte*) noexcept { ++foreign_destroys; }
TEST_CASE("runtime ownership binding preserves deferred signatures and canonical witnesses") {
    const auto number = cel::value_type::leaf<double>();
    const auto declared = cel::value_type::callable(std::array{number}, number);
    const auto bound = cel::bind_runtime_ownership(declared);
    cel::dyn_segment body;
    body.push_argument(0, number);
    cel::callable actual(std::move(body).finish(), std::array{number});
    CHECK_FALSE(declared.owns_values());
    CHECK(bound.owns_values());
    CHECK(bound.same_shape(declared));
    CHECK(bound.compatible_with(actual.type()));
    CHECK_FALSE(declared.compatible_with(actual.type()));
    CHECK(cel::bind_runtime_ownership(bound).compatible_with(bound));
    CHECK(cel::detail::type_access::same_plan(cel::bind_runtime_ownership(bound), bound));
    CHECK(cel::detail::type_access::same_plan(cel::bind_runtime_ownership(actual.type()),
                                              actual.type()));
    CHECK_THROWS_AS(cel::value_type::callable(std::array{bound}, number), std::invalid_argument);
}
TEST_CASE("binding preserves nested nominal layout client identities and range forms") {
    const auto declared = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto client = cel::value_type::leaf<wide_result>();
    const auto range =
        cel::value_type::range(cel::range_kind::range_inclusive, cel::value_type::leaf<double>());
    const auto tuple = cel::value_type::tuple(std::array{client, declared, range});
    const auto record = cel::value_type::record(
        "named", std::array{cel::record_field{"nested", cel::value_type::array(tuple)},
                            cel::record_field{"wide", client}});
    const auto bound = cel::bind_runtime_ownership(record);
    CHECK_FALSE(record.owns_values());
    CHECK(bound.owns_values());
    CHECK(bound.same_shape(record));
    CHECK(bound.record_identity() == "named");
    CHECK(bound.field_names()[0] == "nested");
    CHECK(bound.field_names()[1] == "wide");
    CHECK(bound.offsets()[0] == record.offsets()[0]);
    CHECK(bound.offsets()[1] == record.offsets()[1]);
    CHECK(bound.byte_layout().alignment == 256);
    CHECK(bound.fields()[1].leaf_identity() == typeid(wide_result));
    CHECK(cel::detail::type_access::same_plan(bound.fields()[1], client));
    CHECK(bound.fields()[0].element().fields()[2].form() == cel::range_kind::range_inclusive);
    CHECK_THROWS_AS(cel::value_type::callable(std::array{bound}, client), std::invalid_argument);
    cel::dyn_segment builder;
    builder.make_array(0, bound, {});
    cel::raw_stack stack;
    CHECK(std::move(builder).finish().execute(stack, {}).size() == 0);
}
TEST_CASE("binding rejects foreign operations throughout metadata without invoking owners") {
    const auto declared = cel::value_type::callable({}, cel::value_type::leaf<double>());
    const auto foreign = cel::detail::type_access::install_callable_ownership(
        declared, {clone_foreign, destroy_foreign});
    cel::dyn_segment body;
    body.just(cel::value::from(7.0));
    cel::callable actual(std::move(body).finish(), {});
    const auto aggregate = cel::value_type::tuple(std::array{cel::value_type::array(foreign)});
    const auto nested = cel::value_type::callable({}, aggregate);
    const auto installed_nested = cel::detail::type_access::install_callable_ownership(
        nested, cel::detail::type_access::operations(actual.type()));
    const auto mixed = cel::detail::type_access::install_callable_ownership(
        declared, {cel::detail::type_access::operations(actual.type()).clone, destroy_foreign});
    foreign_clones = 0;
    foreign_destroys = 0;
    for (const auto& input : std::array{foreign, aggregate, nested, installed_nested, mixed}) {
        CHECK_THROWS_AS(cel::bind_runtime_ownership(input), std::invalid_argument);
    }
    CHECK(cel::detail::type_access::operations(foreign).clone == clone_foreign);
    CHECK(cel::detail::type_access::operations(foreign).destroy == destroy_foreign);
    CHECK(foreign_clones == 0);
    CHECK(foreign_destroys == 0);
}
TEST_CASE("binding traverses deep metadata and preserves shared DAGs") {
    auto deep = cel::value_type::callable({}, cel::value_type::leaf<double>());
    for (int i = 0; i != 12000; ++i)
        deep = cel::value_type::callable({}, cel::value_type::array(deep));
    auto bound = cel::bind_runtime_ownership(deep);
    CHECK(bound.same_shape(deep));
    for (int i = 0; i != 12000; ++i) {
        REQUIRE(bound.owns_values());
        REQUIRE(bound.result().owns_values());
        bound = bound.result().element();
    }
    CHECK(bound.owns_values());
    auto shared = cel::value_type::callable({}, cel::value_type::leaf<double>());
    constexpr int depth = 96;
    for (int i = 0; i != depth; ++i) {
        const auto edge = cel::value_type::array(shared);
        shared = cel::value_type::tuple(std::array{edge, edge});
    }
    auto result = cel::bind_runtime_ownership(shared);
    CHECK(result.same_shape(shared));
    for (int i = 0; i != depth; ++i) {
        REQUIRE(result.owns_values());
        CHECK(cel::detail::type_access::same_plan(result.fields()[0], result.fields()[1]));
        result = result.fields()[0].element();
    }
    CHECK(result.owns_values());
}
TEST_CASE("conversion and dynamic invocation reject foreign same shape ownership before access") {
    foreign_clones = 0;
    foreign_destroys = 0;
    int effects = 0;
    cel::dyn_segment body;
    body.just(cel::value::from(true));
    cel::callable fn(std::move(body).finish(), {});
    const auto foreign = cel::detail::type_access::install_callable_ownership(
        fn.type(), {clone_foreign, destroy_foreign});
    alignas(void*) const std::array<std::byte, sizeof(void*)> null_carrier{};
    auto impostor = cel::detail::value_access::take(foreign, null_carrier.data());
    CHECK(impostor.type().same_shape(fn.type()));
    CHECK_THROWS_AS(cel::callable::from_value(impostor), std::invalid_argument);
    CHECK(foreign_clones == 0);
    CHECK(foreign_destroys == 0);
    cel::dyn_segment b;
    b.just(std::move(impostor));
    b.op0<void>([&effects] { ++effects; });
    const auto original = b.snapshot();
    CHECK_THROWS_AS(b.invoke_closure(0), std::invalid_argument);
    CHECK(b.snapshot() == original);
    CHECK(effects == 0);
    CHECK(foreign_clones == 0);
    cel::raw_stack stack;
    auto result = std::move(b).finish().execute(stack, {});
    CHECK(result.type().same_shape(fn.type()));
    CHECK(effects == 1);
    CHECK(foreign_clones == 1);
    CHECK_THROWS_AS(cel::callable::from_value(result), std::invalid_argument);
}
TEST_CASE("higher order results reject foreign ownership before checked execution and binding") {
    for (bool nested : {false, true}) {
        int effects = 0;
        cel::dyn_segment inner_body;
        inner_body.just(cel::value::from(7.0));
        const auto canonical_inner = cel::callable(std::move(inner_body).finish(), {}).to_value();
        const auto foreign_inner = cel::detail::type_access::install_callable_ownership(
            canonical_inner.type(), {clone_foreign, destroy_foreign});
        alignas(void*) const std::array<std::byte, sizeof(void*)> carrier{};
        auto result = cel::detail::value_access::take(foreign_inner, carrier.data());
        if (nested) {
            std::array children{std::move(result)};
            result = cel::value::tuple(children);
        }
        cel::dyn_segment foreign_body;
        foreign_body.just(std::move(result));
        const auto foreign_outer = cel::callable(std::move(foreign_body).finish(), {}).to_value();
        const auto expected_result =
            nested ? cel::value_type::tuple(std::array{canonical_inner.type()}) :
                     canonical_inner.type();
        const auto expected =
            cel::bind_runtime_ownership(cel::value_type::callable({}, expected_result));
        CHECK(foreign_outer.type().same_shape(expected));
        CHECK_FALSE(foreign_outer.type().compatible_with(expected));
        cel::dyn_segment reader;
        reader.push_argument(0, expected);
        reader.op0<void>([&effects] { ++effects; });
        const auto program = std::move(reader).finish();
        cel::raw_stack stack;
        stack.push(true);
        CHECK_THROWS_AS(program.execute(stack, std::array{foreign_outer}), std::invalid_argument);
        CHECK(effects == 0);
        CHECK(stack.depth() == 1);
        CHECK(stack.pop<bool>());
        CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(
                            program, std::array{foreign_outer.type()}, expected),
                        std::invalid_argument);
        CHECK_THROWS_AS(cel::detail::segment_access::prepare_raw(program, std::array{expected},
                                                                 foreign_outer.type()),
                        std::invalid_argument);
        CHECK(effects == 0);
    }
}
} // namespace

#endif
