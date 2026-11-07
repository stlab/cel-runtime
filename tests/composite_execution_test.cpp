// SPDX-License-Identifier: BSL-1.0
#include "allocation_probe.hpp"
#include "type_ops.hpp"
#include "value_ops.hpp"
#include <cel/runtime/dyn_segment.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <array>
#include <doctest/doctest.h>
#include <limits>

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

TEST_CASE("nested packing consumes source order and preserves exact owners") {
    cel::dyn_segment b;
    b.just(cel::value::from(std::vector{2.0, 3.0}));
    b.just(cel::value::from(true));
    b.make_tuple(2, {});
    b.project_tuple(0);
    b.just(cel::value::from(1.0));
    b.index_array();
    cel::raw_stack stack;
    stack.push(std::vector{9.0});
    CHECK(std::move(b).finish().execute(stack, {}).as<double>() == 3.0);
    CHECK(stack.pop<std::vector<double>>() == std::vector{9.0});
}
TEST_CASE("packing admits zero byte fields and empty typed arrays") {
    cel::dyn_segment b;
    b.just(cel::value{});
    b.just(cel::value{});
    b.make_array(2, cel::value_type{}, {});
    cel::raw_stack stack;
    auto result = std::move(b).finish().execute(stack, {});
    CHECK(result.size() == 2);
    CHECK(result.at(1).type().same_shape(cel::value_type{}));
    cel::dyn_segment empty;
    empty.make_array(0, cel::value_type::leaf<double>(), {});
    CHECK(std::move(empty).finish().execute(stack, {}).size() == 0);
}
TEST_CASE("record schema permutation leaves expression effects source ordered") {
    const std::array fields{cel::record_field{"a", cel::value_type::leaf<bool>()},
                            cel::record_field{"z", cel::value_type::leaf<double>()}};
    auto record = cel::value_type::record("sample", fields);
    std::vector<int> order;
    cel::dyn_segment b;
    b.op0<double>([&order] {
        order.push_back(1);
        return 7.0;
    });
    b.op0<bool>([&order] {
        order.push_back(2);
        return true;
    });
    b.make_record(record, std::array<std::size_t, 2>{1, 0}, {});
    cel::raw_stack stack;
    auto result = std::move(b).finish().execute(stack, {});
    CHECK(order == std::vector{1, 2});
    CHECK(result.field("z").as<double>() == 7.0);
    CHECK(result.field("a").as<bool>());
}
TEST_CASE("invalid shapes permutations and ambient origins reject before publication") {
    cel::dyn_segment b;
    b.just(cel::value::from(true));
    CHECK_THROWS_AS(b.make_array(1, cel::value_type::leaf<double>(), {}), std::invalid_argument);
    CHECK_THROWS_AS(b.make_tuple(2, {}), std::invalid_argument);
    CHECK_THROWS_AS(b.make_tuple(1, {0, 1}), std::invalid_argument);
    const std::array fields{cel::record_field{"a", cel::value_type::leaf<bool>()}};
    auto record = cel::value_type::record("sample", fields);
    CHECK_THROWS_AS(b.make_record(record, std::array<std::size_t, 1>{1}, {}),
                    std::invalid_argument);
    b.make_record(record, {});
    b.project_record("a");
    cel::raw_stack stack;
    CHECK(std::move(b).finish().execute(stack, {}).as<bool>());
}
TEST_CASE("every range kind retains endpoint metadata and source order") {
    for (auto kind : {cel::range_kind::range, cel::range_kind::range_inclusive,
                      cel::range_kind::range_from, cel::range_kind::range_to,
                      cel::range_kind::range_to_inclusive, cel::range_kind::range_full}) {
        const auto type = cel::value_type::range(kind, cel::value_type::leaf<double>());
        cel::dyn_segment b;
        for (std::size_t i = 0; i != type.offsets().size(); ++i)
            b.just(cel::value::from(2.0 + static_cast<double>(i)));
        b.make_range(kind);
        cel::raw_stack stack;
        auto result = std::move(b).finish().execute(stack, {});
        CHECK(result.type().same_shape(type));
        for (std::size_t i = 0; i != result.size(); ++i)
            CHECK(result.at(i).as<double>() == 2.0 + static_cast<double>(i));
    }
}
TEST_CASE("invalid floating indices reject safely and restore prefixes") {
    for (double index :
         {std::numeric_limits<double>::quiet_NaN(), std::numeric_limits<double>::infinity(), -1.0,
          0.5, static_cast<double>(std::numeric_limits<std::size_t>::max()), 2.0}) {
        cel::dyn_segment b;
        b.just(cel::value::from(std::vector{1.0}));
        b.just(cel::value::from(index));
        b.index_array();
        auto program = std::move(b).finish();
        cel::raw_stack stack;
        stack.push(true);
        CHECK_THROWS_AS(program.execute(stack, {}), std::out_of_range);
        CHECK(stack.pop<bool>());
    }
}
TEST_CASE("lazy joins select one body and preserve dependent origin laws") {
    cel::dyn_segment b;
    b.just(cel::value::from(std::vector{4.0}));
    b.just(cel::value::from(true));
    auto origin = b.before_top();
    cel::dyn_segment yes(origin), no(origin);
    yes.just(cel::value::from(5.0));
    no.op0<double>([]() -> double { throw std::runtime_error("unselected"); });
    b.if_else(std::move(yes), std::move(no));
    b.make_tuple(2, {});
    cel::raw_stack stack;
    CHECK(std::move(b).finish().execute(stack, {}).at(1).as<double>() == 5.0);
    cel::dyn_segment dependent(origin);
    dependent.just(cel::value::from(true));
    auto program = std::move(dependent).finish();
    CHECK_THROWS_AS(program.execute(stack, {}), std::invalid_argument);
    CHECK_THROWS_AS(program.prepare<bool>(), std::invalid_argument);
}
TEST_CASE("logical lazy rhs reads are preflighted even when unselected") {
    cel::dyn_segment b;
    b.just(cel::value::from(false));
    cel::dyn_segment rhs(b.before_top());
    rhs.push_argument(2, cel::value_type::leaf<bool>());
    b.logical_and(std::move(rhs));
    auto program = std::move(b).finish();
    cel::raw_stack stack;
    CHECK_THROWS_AS(program.execute(stack, {}), std::invalid_argument);
    const std::array args{cel::value{}, cel::value{}, cel::value::from(true)};
    CHECK_FALSE(program.execute(stack, args).as<bool>());
}
TEST_CASE("logical or executes rhs only for false left operands") {
    for (bool condition : {false, true}) {
        int effects = 0;
        cel::dyn_segment b;
        b.just(cel::value::from(condition));
        cel::dyn_segment rhs(b.before_top());
        rhs.op0<bool>([&effects, condition] {
            if (condition) throw std::runtime_error("unselected logical rhs");
            ++effects;
            return true;
        });
        b.logical_or(std::move(rhs));
        cel::raw_stack stack;
        CHECK(std::move(b).finish().execute(stack, {}).as<bool>());
        CHECK(effects == (condition ? 0 : 1));
    }
}
TEST_CASE("lazy join rejects wrong result origins and conflicting shared reads before effects") {
    cel::dyn_segment b;
    b.just(cel::value::from(true));
    cel::dyn_segment yes(b.before_top()), no(b.before_top());
    yes.push_argument(0, cel::value_type::leaf<bool>());
    no.push_argument(0, cel::value_type::leaf<double>());
    no.op1<double, bool>([](double n) { return n != 0; });
    CHECK_THROWS_AS(b.if_else(std::move(yes), std::move(no)), std::invalid_argument);
    cel::dyn_segment wrong_result(b.before_top()), good(b.before_top());
    wrong_result.just(cel::value::from(1.0));
    good.just(cel::value::from(true));
    CHECK_THROWS_AS(b.if_else(std::move(good), std::move(wrong_result)), std::invalid_argument);
    cel::dyn_segment wrong_origin({0, 1}), right(b.before_top());
    wrong_origin.just(cel::value::from(true));
    right.just(cel::value::from(true));
    CHECK_THROWS_AS(b.if_else(std::move(right), std::move(wrong_origin)), std::invalid_argument);
    cel::raw_stack stack;
    CHECK(std::move(b).finish().execute(stack, {}).as<bool>());
}
TEST_CASE("lazy requirements retain zero byte prefix depth and maximum selected branch peak") {
    cel::dyn_segment b;
    b.just(cel::value{});
    b.just(cel::value::from(true));
    cel::dyn_segment yes(b.before_top()), no(b.before_top());
    for (int i = 0; i != 10; ++i)
        yes.just(cel::value{});
    yes.make_tuple(10, yes.origin());
    for (int i = 0; i != 7; ++i)
        no.just(cel::value{});
    no.make_tuple(7, no.origin());
    no.op1<std::tuple<std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>,
                      std::tuple<>, std::tuple<>>,
           std::tuple<std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>,
                      std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>>>(
        [](auto) {
            return std::tuple<std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>,
                              std::tuple<>, std::tuple<>, std::tuple<>, std::tuple<>,
                              std::tuple<>>{};
        });
    b.if_else(std::move(yes), std::move(no));
    b.make_tuple(2, {});
    auto program = std::move(b).finish();
    CHECK(program.requirements().live_slots == 11);
    cel::raw_stack stack;
    const auto required = program.requirements().for_prefix(0, 0);
    stack.reserve(required.bytes.size, required.bytes.alignment);
    stack.reserve_depth(required.slots);
    CHECK(program.execute(stack, {}).at(1).size() == 10);
}
TEST_CASE("nested record array packing and projection cleanup survive allocation failures") {
    const auto array = cel::value_type::array(cel::value_type::leaf<double>());
    const std::array fields{cel::record_field{"a", array}, cel::record_field{"b", array}};
    const auto record = cel::value_type::record("owned", fields);
    cel::dyn_segment b;
    b.just(cel::value::from(std::vector{2.0}));
    b.just(cel::value::from(std::vector{3.0}));
    b.make_record(record, std::array<std::size_t, 2>{1, 0}, {});
    b.make_array(1, record, {});
    b.just(cel::value::from(0.0));
    b.index_array();
    b.project_record("b");
    auto program = std::move(b).finish();
    cel::raw_stack stack;
    stack.push(std::vector{9.0});
    for (int fail = 0; fail != 40; ++fail) {
        allocation_failure_after = fail;
        try {
            auto result = program.execute(stack, {});
            allocation_failure_after = -1;
            CHECK(result.as<std::vector<double>>() == std::vector{2.0});
        } catch (const std::bad_alloc&) {
            allocation_failure_after = -1;
        }
        CHECK(stack.depth() == 1);
        CHECK(program.execute(stack, {}).as<std::vector<double>>() == std::vector{2.0});
    }
    CHECK(stack.pop<std::vector<double>>() == std::vector{9.0});
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
        for (int fail = 0; fail != 24; ++fail) {
            allocation_failure_after = fail;
            try {
                auto result = program.execute(stack, {});
                allocation_failure_after = -1;
                CHECK(result.type().same_shape(signature));
            } catch (const std::bad_alloc&) {
                allocation_failure_after = -1;
            }
            CHECK(opaque_owners == 2);
            CHECK(stack.depth() == 0);
        }
    }
    CHECK(opaque_owners == 0);
}
