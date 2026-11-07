// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/dyn_segment.hpp>

/// Supplies a named overload set with no boolean operand signature.
struct overloaded {
    /// Returns its exact floating-point operand.
    double operator()(double number) const { return number; }
    /// Adds two exact floating-point operands.
    double operator()(double a, double b) const { return a + b; }
};

/// Rejects implicit CEL operand conversion through an ambiguous named overload set.
int main() {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.op1<bool, double>(overloaded{});
}
