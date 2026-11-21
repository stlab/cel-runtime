// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/raw_stack.hpp>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <tuple>
#include <vector>
/// Verifies packed ownership can cross the raw boundary without aliasing or double release.
TEST_CASE("packed composites transfer to and from reusable stacks with complete metadata") {
    cel::raw_stack stack;
    auto owner = cel::value::from(std::tuple{true, std::vector<std::vector<double>>{{2.0}, {4.0}}});
    stack.push_clone(owner);
    owner.set(1, cel::value::from(std::vector<std::vector<double>>{{9.0}}));
    auto extracted = stack.pop_value();
    CHECK(extracted.at(1).at(0).at(0).as<double>() == 2.0);
    stack.push_take(extracted);
    CHECK(extracted.size() == 0);
    cel::raw_stack moved(std::move(stack));
    CHECK(moved.depth() == 1);
    CHECK(moved.pop_value().at(1).at(1).at(0).as<double>() == 4.0);
    cel::raw_stack assigned;
    assigned.push(std::vector{1.0});
    assigned = std::move(moved);
    CHECK(assigned.depth() == 0);
}
/// Catches conservative type-capability rejection of valid uninhabited packed owners.
TEST_CASE("empty deferred-element arrays retain their legal ownership across raw transfers") {
    const auto deferred = cel::value_type::callable({}, cel::value_type::leaf<double>());
    auto empty = cel::value::array(deferred, {});
    cel::raw_stack stack;
    stack.push_clone(empty);
    CHECK(stack.pop_value().size() == 0);
    stack.push_take(empty);
    CHECK(empty.size() == 0);
    CHECK(stack.pop_value().type().element().kind() == cel::type_kind::callable);
    CHECK(stack.depth() == 0);
}
