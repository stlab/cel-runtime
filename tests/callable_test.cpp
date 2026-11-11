// SPDX-License-Identifier: BSL-1.0
#include "instruction.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <cel/runtime.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <array>
#include <doctest/doctest.h>

/// Supplies a valid client-defined closure result, not a parameter.
struct client_result {
    int number; ///< Retains client data.
};
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
TEST_CASE("bound empty callable arrays retain ownership beyond compiler metadata lifetimes") {
    const auto declared = cel::value_type::callable({}, cel::value_type::leaf<double>());
    cel::dyn_segment rejected;
    CHECK_THROWS_AS(rejected.make_array(0, declared, {}), std::invalid_argument);
    const auto compile = [&] {
        cel::dyn_segment builder;
        builder.make_array(0, cel::bind_runtime_ownership(declared), {});
        return std::move(builder).finish();
    };
    cel::raw_stack stack;
    auto result = compile().execute(stack, {});
    auto copied = result;
    CHECK(result.size() == 0);
    CHECK(copied.size() == 0);
    CHECK(copied.type().owns_values());
    cel::dyn_segment literal;
    literal.just(copied);
    CHECK(std::move(literal).finish().execute(stack, {}).size() == 0);
    cel::dyn_segment unowned_literal;
    CHECK_THROWS_AS(unowned_literal.just(cel::value::array(declared, {})), std::invalid_argument);
    cel::dyn_segment closure_body;
    closure_body.just(result);
    cel::callable factory(std::move(closure_body).finish(), {});
    CHECK(factory.call(stack, {}).size() == 0);
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
TEST_CASE("binding covers higher order result witnesses and populated callable arrays") {
    const auto number = cel::value_type::leaf<double>();
    const auto inner_type = cel::value_type::callable({}, number);
    const auto higher = cel::value_type::callable({}, inner_type);
    const auto bound = cel::bind_runtime_ownership(higher);
    CHECK(bound.result().owns_values());
    CHECK_FALSE(higher.result().owns_values());
    cel::dyn_segment inner;
    inner.just(cel::value::from(7.0));
    cel::dyn_segment outer;
    outer.push_closure(std::move(inner).finish(), {});
    cel::callable actual(std::move(outer).finish(), {});
    CHECK(bound.compatible_with(actual.type()));
    CHECK(bound.result().compatible_with(actual.type().result()));
    cel::raw_stack stack;
    CHECK(cel::callable::from_value(actual.call(stack, {})).call(stack, {}).as<double>() == 7.0);
    cel::dyn_segment populated;
    populated.just(actual.to_value());
    populated.make_array(1, bound, {});
    auto array = std::move(populated).finish().execute(stack, {});
    CHECK(array.size() == 1);
    CHECK(cel::callable::from_value(cel::callable::from_value(array.at(0)).call(stack, {}))
              .call(stack, {})
              .as<double>() == 7.0);
    const auto result_record = cel::value_type::record(
        "result", std::array{cel::record_field{"functions", cel::value_type::array(higher)}});
    const auto complete = cel::bind_runtime_ownership(cel::value_type::callable({}, result_record));
    CHECK(complete.result().owns_values());
    CHECK(complete.result().fields()[0].element().result().owns_values());
    cel::dyn_segment empty;
    empty.make_array(0, bound, {});
    CHECK(std::move(empty).finish().execute(stack, {}).size() == 0);
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
TEST_CASE("callable full declarations include unused composite parameters") {
    int effects = 0;
    cel::dyn_segment b;
    b.op0<client_result>([&effects] {
        ++effects;
        return client_result{8};
    });
    const std::array signature{cel::value_type::array(cel::value_type::leaf<double>())};
    cel::callable fn(std::move(b).finish(), signature);
    cel::raw_stack stack;
    CHECK_THROWS_AS(fn.call(stack, std::array{cel::value::from(true)}), std::invalid_argument);
    CHECK_THROWS_AS(fn.call(stack, {}), std::invalid_argument);
    CHECK(effects == 0);
    CHECK(
        fn.call(stack, std::array{cel::value::from(std::vector{2.0})}).as<client_result>().number ==
        8);
    CHECK(effects == 1);
}
TEST_CASE("installed public conversions receive and invoke closure results") {
    cel::dyn_segment inner;
    inner.push_argument(0, cel::value_type::leaf<double>());
    inner.op1<double, double>([](double n) { return n + 1.0; });
    cel::dyn_segment outer;
    outer.push_closure(std::move(inner).finish(), std::array{cel::value_type::leaf<double>()});
    cel::callable factory(std::move(outer).finish(), {});
    cel::raw_stack stack;
    auto fn = cel::callable::from_value(factory.call(stack, {}));
    CHECK(fn.call(stack, std::array{cel::value::from(4.0)}).as<double>() == 5.0);
    CHECK(cel::callable::from_value(fn.to_value())
              .call(stack, std::array{cel::value::from(2.0)})
              .as<double>() == 3.0);
}
TEST_CASE("nested invocation stages arguments independently of growing main stack") {
    cel::raw_stack stack;
    cel::dyn_segment inner;
    inner.push_argument(0, cel::value_type::array(cel::value_type::leaf<double>()));
    inner.op1<std::vector<double>, std::vector<double>>([&stack](std::vector<double> input) {
        cel::dyn_segment huge;
        for (int i = 0; i != 1024; ++i)
            huge.just(cel::value::from(3.0));
        huge.make_array(1024, cel::value_type::leaf<double>(), {});
        CHECK(std::move(huge).finish().execute(stack, {}).size() == 1024);
        return input;
    });
    inner.op<void, std::vector<double>>([](std::vector<double>) {});
    inner.push_argument(0, cel::value_type::array(cel::value_type::leaf<double>()));
    cel::dyn_segment outer;
    outer.just(cel::value::from(std::vector{9.0}));
    outer.push_closure(std::move(inner).finish(),
                       std::array{cel::value_type::array(cel::value_type::leaf<double>())});
    outer.just(cel::value::from(std::vector{2.0, 4.0}));
    outer.invoke_closure(1);
    outer.make_tuple(2, {});
    auto result = std::move(outer).finish().execute(stack, {});
    CHECK(result.at(0).as<std::vector<double>>() == std::vector{9.0});
    CHECK(result.at(1).as<std::vector<double>>() == std::vector{2.0, 4.0});
    CHECK(stack.depth() == 0);
}
TEST_CASE("composite parameters and nested callable owning results retain complete signatures") {
    const std::array fields{
        cel::record_field{"items", cel::value_type::array(cel::value_type::leaf<double>())}};
    const auto record = cel::value_type::record("argument", fields);
    cel::dyn_segment b;
    b.push_argument(0, record);
    b.project_record("items");
    b.make_tuple(1, {});
    const std::array declarations{record, cel::value_type::tuple({})};
    cel::callable fn(std::move(b).finish(), declarations);
    cel::raw_stack stack;
    const std::array args{
        cel::value::record(record, std::array{cel::value::from(std::vector{6.0})}), cel::value{}};
    CHECK(fn.type().parameters().size() == 2);
    CHECK(fn.call(stack, args).at(0).as<std::vector<double>>() == std::vector{6.0});
    auto wrong = cel::value_type::record("different", fields);
    const std::array bad{cel::value::record(wrong, std::array{cel::value::from(std::vector{6.0})}),
                         cel::value{}};
    CHECK_THROWS_AS(fn.call(stack, bad), std::invalid_argument);
    cel::dyn_segment nested;
    nested.just(cel::value::from(true));
    cel::dyn_segment owner;
    owner.push_closure(std::move(nested).finish(), {});
    owner.make_tuple(1, {});
    cel::callable factory(std::move(owner).finish(), {});
    CHECK(cel::callable::from_value(factory.call(stack, {}).at(0)).call(stack, {}).as<bool>());
}
TEST_CASE("nested invocation preserves owning results and caller prefix") {
    cel::dyn_segment body;
    body.push_argument(0, cel::value_type::array(cel::value_type::leaf<double>()));
    body.make_tuple(1, {});
    cel::dyn_segment b;
    b.push_closure(std::move(body).finish(),
                   std::array{cel::value_type::array(cel::value_type::leaf<double>())});
    b.just(cel::value::from(std::vector{2.0, 5.0}));
    b.invoke_closure(1);
    auto program = std::move(b).finish();
    cel::raw_stack stack;
    const auto reservation = program.requirements().for_prefix(0, 0);
    stack.reserve(reservation.bytes.size + 128, reservation.bytes.alignment);
    stack.push(std::vector{9.0});
    for (int repeat = 0; repeat != 2; ++repeat) {
        auto result = program.execute(stack, {});
        CHECK(result.at(0).as<std::vector<double>>() == std::vector{2.0, 5.0});
        CHECK(stack.depth() == 1);
    }
    CHECK(stack.pop<std::vector<double>>() == std::vector{9.0});
}
TEST_CASE("reserved lazy scalar dispatch and callable invocation reuse the stack") {
    cel::dyn_segment b;
    b.push_argument(0, cel::value_type::leaf<bool>());
    cel::dyn_segment rhs(b.before_top());
    rhs.just(cel::value::from(true));
    b.logical_and(std::move(rhs));
    auto scalar = std::move(b).finish().prepare<bool, bool>();
    cel::raw_stack stack;
    const auto required = scalar.requirements().for_prefix(0, 0);
    stack.reserve(required.bytes.size, required.bytes.alignment);
    stack.reserve_depth(required.slots);
    CHECK(scalar.call(stack, true));
    for (int i = 0; i != 100; ++i)
        CHECK(scalar.call(stack, true));
    cel::dyn_segment body;
    body.push_argument(0, cel::value_type::leaf<double>());
    cel::dyn_segment outer;
    outer.push_closure(std::move(body).finish(), std::array{cel::value_type::leaf<double>()});
    outer.just(cel::value::from(4.0));
    outer.invoke_closure(1);
    auto invoke = std::move(outer).finish().prepare<double>();
    CHECK(invoke.call(stack) == 4.0);
    CHECK(invoke.call(stack) == 4.0);
}
TEST_CASE("same carrier reentry rejects and throwing bodies preserve stack reuse") {
    cel::raw_stack stack;
    const cel::callable* self = nullptr;
    cel::dyn_segment b;
    b.op0<bool>([&] {
        CHECK_THROWS_AS(self->call(stack, {}), std::logic_error);
        throw std::runtime_error("original body");
        return true;
    });
    cel::callable fn(std::move(b).finish(), {});
    auto alias = fn;
    self = &alias;
    stack.push(std::vector{7.0});
    CHECK_THROWS_WITH(fn.call(stack, {}), "original body");
    CHECK_THROWS_WITH(fn.call(stack, {}), "original body");
    CHECK(stack.pop<std::vector<double>>() == std::vector{7.0});
}
TEST_CASE("direct known callee requirements include intermediate alignment and extent") {
    cel::dyn_segment body;
    body.op0<wide_result>([] { return wide_result{true}; });
    body.op1<bool, wide_result>([](const wide_result& input) { return input.flag; });
    cel::dyn_segment b;
    b.push_closure(std::move(body).finish(), {});
    b.invoke_closure(0);
    auto program = std::move(b).finish();
    CHECK(program.requirements().alignment == 256);
    CHECK(program.requirements().frame_extent >= sizeof(wide_result));
    cel::raw_stack stack;
    const auto reservation = program.requirements().for_prefix(0, 0);
    stack.reserve(reservation.bytes.size, reservation.bytes.alignment);
    stack.reserve_depth(reservation.slots);
    CHECK(program.prepare<bool>().call(stack));
}
TEST_CASE("distinct callable carriers nest inside a guarded dynamic invocation") {
    int inner_effects = 0, outer_effects = 0;
    cel::raw_stack stack;
    stack.push(std::vector{6.0});
    cel::dyn_segment inner;
    inner.op0<bool>([&inner_effects] {
        ++inner_effects;
        return true;
    });
    cel::callable inner_fn(std::move(inner).finish(), {});
    cel::dyn_segment outer;
    outer.op0<bool>([&] {
        ++outer_effects;
        return inner_fn.call(stack, {}).as<bool>();
    });
    cel::callable outer_fn(std::move(outer).finish(), {});
    cel::dyn_segment b;
    b.just(outer_fn.to_value());
    b.invoke_closure(0);
    CHECK(std::move(b).finish().execute(stack, {}).as<bool>());
    CHECK(inner_effects == 1);
    CHECK(outer_effects == 1);
    CHECK(stack.pop<std::vector<double>>() == std::vector{6.0});
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
TEST_CASE("closure construction validates reads parameters and independence") {
    cel::dyn_segment b;
    b.push_argument(1, cel::value_type::leaf<bool>());
    CHECK_THROWS_AS(cel::callable(std::move(b).finish(), std::array{cel::value_type::leaf<bool>()}),
                    std::invalid_argument);
    cel::dyn_segment dependent({0, 1});
    dependent.just(cel::value::from(true));
    CHECK_THROWS_AS(cel::callable(std::move(dependent).finish(), {}), std::invalid_argument);
    cel::dyn_segment leaf;
    leaf.just(cel::value::from(true));
    CHECK_THROWS_AS(
        cel::callable(std::move(leaf).finish(), std::array{cel::value_type::leaf<client_result>()}),
        std::invalid_argument);
}
TEST_CASE("dynamic call rejects exact signature mismatches before emitted callback effects") {
    int effects = 0;
    cel::dyn_segment body;
    body.just(cel::value::from(true));
    cel::dyn_segment b;
    b.push_closure(std::move(body).finish(), std::array{cel::value_type::leaf<double>()});
    b.op0<bool>([&effects] {
        ++effects;
        return false;
    });
    CHECK_THROWS_AS(b.invoke_closure(0), std::invalid_argument);
    CHECK_THROWS_AS(b.invoke_closure(1), std::invalid_argument);
    CHECK(effects == 0);
    b.make_tuple(2, {});
    cel::raw_stack stack;
    CHECK(std::move(b).finish().execute(stack, {}).size() == 2);
    CHECK(effects == 1);
}
TEST_CASE("throwing dynamic bodies retire staged composites without replacing exceptions") {
    cel::dyn_segment body;
    body.push_argument(0, cel::value_type::array(cel::value_type::leaf<double>()));
    body.op1<double, std::vector<double>>([](std::vector<double>) -> double {
        throw std::runtime_error("specific nested body context");
    });
    cel::dyn_segment b;
    b.push_closure(std::move(body).finish(),
                   std::array{cel::value_type::array(cel::value_type::leaf<double>())});
    b.just(cel::value::from(std::vector{4.0}));
    b.invoke_closure(1);
    auto program = std::move(b).finish();
    cel::raw_stack stack;
    stack.push(std::vector{7.0});
    CHECK_THROWS_WITH(program.execute(stack, {}), "specific nested body context");
    CHECK_THROWS_WITH(program.execute(stack, {}), "specific nested body context");
    CHECK(stack.pop<std::vector<double>>() == std::vector{7.0});
}
