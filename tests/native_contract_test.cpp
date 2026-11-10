// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

namespace {
/// Exercises over-aligned client leaf descriptors and canonical padding.
struct alignas(32) extension {
    double number;
};
} // namespace

TEST_CASE("native descriptors bind scalars extensions and nested structural values") {
    CHECK(cel::native_type<bool>().compatible_with(cel::value::from(true).type()));
    CHECK(cel::native_type<double>().compatible_with(cel::value::from(1.0).type()));
    CHECK(cel::native_type<const extension&>().compatible_with(
        cel::value::from(extension{3.0}).type()));
    using nested = std::tuple<std::tuple<>, std::vector<std::tuple<bool, extension>>>;
    const nested input{{}, {{true, extension{4.0}}}};
    cel::dyn_segment builder;
    builder.push_argument(0, cel::native_type<nested>());
    auto bound = std::move(builder).finish().prepare<nested, nested>();
    cel::raw_stack stack;
    const auto result = bound.call(stack, input);
    CHECK(std::get<1>(std::get<1>(result)[0]).number == 4.0);
    CHECK(cel::native_type<std::tuple<>>().compatible_with(cel::value{}.type()));
}

TEST_CASE("cached origins cover empty suffixes alignment removal and structural changes") {
    cel::dyn_segment builder({3, 5});
    CHECK(builder.origin_below(0) == cel::stack_origin{3, 5});
    CHECK_THROWS_AS(builder.origin_below(1), std::invalid_argument);
    builder.just(cel::value::from(true));
    CHECK(builder.origin_below(0) == cel::stack_origin{4, 6});
    builder.just(cel::value::from(extension{2.0}));
    CHECK(builder.origin_below(0) == cel::stack_origin{64, 7});
    CHECK(builder.origin_below(1) == cel::stack_origin{4, 6});
    CHECK(builder.origin_below(2) == cel::stack_origin{3, 5});
    CHECK_THROWS_AS(builder.origin_below(3), std::invalid_argument);
    builder.op1<void, extension>([](extension) {});
    CHECK(builder.origin_below(0) == cel::stack_origin{4, 6});
    builder.just(cel::value{});
    CHECK(builder.origin_below(0) == cel::stack_origin{4, 7});
    builder.make_tuple(2, builder.origin_below(2));
    CHECK(builder.origin_below(1) == cel::stack_origin{3, 5});
    builder.project_tuple(0);
    CHECK(builder.origin_below(0) == cel::stack_origin{4, 6});
    builder.make_array(1, cel::native_type<bool>(), builder.origin_below(1));
    CHECK(builder.origin_below(1) == cel::stack_origin{3, 5});
    CHECK(builder.before_top() == builder.origin_below(1));
    CHECK(builder.snapshot() == builder.origin_below(0));
}

TEST_CASE("arity helpers select result before asymmetric source ordered operands") {
    cel::dyn_segment builder;
    builder.just(cel::value::from(2.0));
    builder.op1<bool, double>([](double n) { return n == 2.0; });
    builder.just(cel::value::from(7.0));
    builder.op2<extension, bool, double>(
        [](bool flag, double n) { return extension{flag ? n : 0.0}; });
    builder.just(cel::value::from(true));
    builder.just(cel::value::from(3.0));
    builder.op3<double, extension, bool, double>(
        [](extension older, bool flag, double newer) { return flag ? older.number - newer : 0.0; });
    auto bound = std::move(builder).finish().prepare<double>();
    cel::raw_stack stack;
    CHECK(bound.call(stack) == 4.0);
}
