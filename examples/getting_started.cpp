// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>

#include <exception>
#include <iostream>

/// Compiles one program, prepares its exact signature, and reuses a reserved caller stack.
/// Returns zero after verifying results and preserved caller-owned prefix storage.
int main() try {
    cel::dyn_segment builder;
    builder.push_argument(0, cel::value_type::leaf<double>());
    builder.just(cel::value::from(3.0));
    builder.op2<double, double, double>([](double older, double newer) { return older - newer; });
    auto program = std::move(builder).finish();
    auto prepared = program.prepare<double, double>();

    cel::raw_stack stack;
    stack.push(true);
    const auto needed = prepared.requirements().for_prefix(stack.cursor(), stack.depth());
    stack.reserve(needed.bytes.size, needed.bytes.alignment);
    stack.reserve_depth(needed.slots);

    const auto first = prepared.call(stack, 8.0);
    const auto second = prepared.call(stack, 10.0);
    if (first != 5.0 || second != 7.0 || !stack.pop<bool>() || stack.depth() != 0) return 1;
    std::cout << first << '\n' << second << '\n';
    return 0;
} catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 2;
} catch (...) {
    std::cerr << "Unexpected non-standard exception\n";
    return 3;
}
