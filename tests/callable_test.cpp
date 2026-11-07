// SPDX-License-Identifier: BSL-1.0
#include "allocation_probe.hpp"
#include <cel/runtime/callable.hpp>
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
/// Borrows the callable used by a bounded acquisition-reentry test.
const cel::callable* acquisition_callable = nullptr;
/// Borrows its caller-owned stack.
cel::raw_stack* acquisition_stack = nullptr;
/// Records observable same-carrier rejection.
bool acquisition_rejected = false;
/// Attempts one same-carrier call during invocation-storage acquisition.
void acquisition_reentry() {
    try {
        (void)acquisition_callable->call(*acquisition_stack, {});
    } catch (const std::logic_error&) {
        acquisition_rejected = true;
    }
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
TEST_CASE("nested invocation allocation failures release staged owners and permit reuse") {
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
    for (int fail = 0; fail != 40; ++fail) {
        allocation_failure_after = fail;
        bool failed = false;
        try {
            auto result = program.execute(stack, {});
            allocation_failure_after = -1;
            CHECK(result.at(0).as<std::vector<double>>() == std::vector{2.0, 5.0});
        } catch (const std::bad_alloc&) {
            allocation_failure_after = -1;
            failed = true;
        }
        CHECK(stack.depth() == 1);
        if (failed)
            CHECK(program.execute(stack, {}).at(0).as<std::vector<double>>() ==
                  std::vector{2.0, 5.0});
    }
    CHECK(stack.pop<std::vector<double>>() == std::vector{9.0});
}
TEST_CASE("reserved lazy scalar dispatch remains allocation free while invocation stages owners") {
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
    {
        allocation_probe probe;
        for (int i = 0; i != 100; ++i)
            (void)scalar.call(stack, true);
    }
    CHECK(allocation_count == 0);
    cel::dyn_segment body;
    body.push_argument(0, cel::value_type::leaf<double>());
    cel::dyn_segment outer;
    outer.push_closure(std::move(body).finish(), std::array{cel::value_type::leaf<double>()});
    outer.just(cel::value::from(4.0));
    outer.invoke_closure(1);
    auto invoke = std::move(outer).finish().prepare<double>();
    CHECK(invoke.call(stack) == 4.0);
    {
        allocation_probe probe;
        (void)invoke.call(stack);
    }
    MESSAGE("warmed scalar callable invocation allocations: ", allocation_count);
    CHECK(allocation_count > 0);
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
    body.op1<wide_result, bool>([](wide_result input) { return input.flag; });
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
TEST_CASE("public checked calls reject same carrier entry throughout invocation acquisition") {
    int effects = 0;
    cel::dyn_segment b;
    b.op0<bool>([&effects] {
        ++effects;
        return true;
    });
    cel::callable fn(std::move(b).finish(), {});
    cel::raw_stack stack;
    acquisition_callable = &fn;
    acquisition_stack = &stack;
    acquisition_rejected = false;
    allocation_hook = acquisition_reentry;
    CHECK(fn.call(stack, {}).as<bool>());
    CHECK(acquisition_rejected);
    CHECK(effects == 1);
    acquisition_callable = nullptr;
    acquisition_stack = nullptr;
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
    body.op1<std::vector<double>, double>([](std::vector<double>) -> double {
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
