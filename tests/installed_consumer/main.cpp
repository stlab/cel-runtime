// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>

#include <array>
#include <tuple>
#include <vector>

/// Verifies installed scalar, independent nested-owner, and surviving callable results.
/// Returns zero when cached execution preserves the caller prefix and all results match.
int main() try {
    if (!cel::value_type::leaf<double>().compatible_with(cel::value_type::leaf<double>()) ||
        cel::value_type::leaf<double>().compatible_with(cel::value_type::leaf<bool>()))
        return 9;
    cel::raw_stack stack;
    stack.push(true);
    const auto cursor = stack.cursor();
    const auto depth = stack.depth();

    cel::dyn_segment scalar;
    scalar.push_argument(0, cel::value_type::leaf<double>());
    const auto operands = scalar.operand_types(1);
    if (operands.size() != 1 || !operands[0].compatible_with(cel::value_type::leaf<double>()) ||
        operands[0].compatible_with(cel::value_type::leaf<bool>()) ||
        !scalar.operand_types(2).empty())
        return 8;
    scalar.op1<double, double>([](double n) { return n * 2.0; });
    auto cached = std::move(scalar).finish().prepare<double, double>();
    const auto needed = cached.requirements().for_prefix(cursor, depth);
    stack.reserve(needed.bytes.size, needed.bytes.alignment);
    stack.reserve_depth(needed.slots);
    if (cached.call(stack, 3.0) != 6.0 || cached.call(stack, 4.0) != 8.0) return 1;

    using nested = std::tuple<bool, std::vector<std::tuple<double, std::vector<bool>>>>;
    const nested input{true, {{2.0, {true, false}}}};
    cel::dyn_segment composite;
    composite.push_argument(0, cel::value::from(input).type());
    auto copy = std::move(composite).finish().prepare<nested, nested>();
    auto first = copy.call(stack, input);
    auto second = copy.call(stack, input);
    std::get<1>(std::get<1>(first)[0])[0] = false;
    if (!std::get<1>(std::get<1>(second)[0])[0] || !std::get<1>(std::get<1>(input)[0])[0]) return 2;

    auto received = [&] {
        cel::dyn_segment body;
        body.push_argument(0, cel::value_type::leaf<double>());
        body.op1<double, double>([](double n) { return n + 1.0; });
        cel::dyn_segment factory;
        factory.push_closure(std::move(body).finish(), std::array{cel::value_type::leaf<double>()});
        return std::move(factory).finish().execute(stack, {});
    }();
    auto fn = cel::callable::from_value(received);
    received = cel::value{};
    if (fn.call(stack, std::array{cel::value::from(4.0)}).as<double>() != 5.0) return 3;
    auto alias = cel::callable::from_value(fn.to_value());
    if (alias.call(stack, std::array{cel::value::from(8.0)}).as<double>() != 9.0) return 4;
    const auto declared = cel::value_type::callable(std::array{cel::value_type::leaf<double>()},
                                                    cel::value_type::leaf<double>());
    const auto bound = cel::bind_runtime_ownership(declared);
    if (declared.owns_values() || !bound.owns_values() || !bound.compatible_with(fn.type()) ||
        !cel::bind_runtime_ownership(bound).compatible_with(bound))
        return 10;
    auto empty = [&] {
        const auto higher = cel::value_type::callable({}, declared);
        const auto record = cel::value_type::record(
            "empty", std::array{cel::record_field{"factory", higher},
                                cel::record_field{"functions", cel::value_type::array(declared)}});
        cel::dyn_segment builder;
        builder.make_array(0, cel::bind_runtime_ownership(record), {});
        return std::move(builder).finish().execute(stack, {});
    }();
    auto empty_copy = empty;
    if (empty.size() != 0 || empty_copy.size() != 0 ||
        !empty_copy.type().element().fields()[0].result().compatible_with(fn.type()))
        return 11;
    cel::dyn_segment populated;
    populated.just(fn.to_value());
    populated.make_array(1, bound, {});
    auto functions = std::move(populated).finish().execute(stack, {});
    if (cel::callable::from_value(functions.at(0))
            .call(stack, std::array{cel::value::from(6.0)})
            .as<double>() != 7.0)
        return 12;
    if (stack.cursor() != cursor || stack.depth() != depth || !stack.pop<bool>()) return 5;
    return stack.depth() == 0 ? 0 : 6;
} catch (...) {
    return 7;
}
