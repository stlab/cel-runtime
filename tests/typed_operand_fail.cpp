// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/dyn_segment.hpp>
/// Rejects an explicitly declared operand that differs from the callback's concrete argument.
int main() {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.op1<double, bool>([](double number) { return number; });
}
