// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/dyn_segment.hpp>

/// Supplies a template whose declared operand remains double for every specialization.
struct fixed_generic {
    /// Returns the exact floating-point operand, independently of T.
    template <class T = void>
    double operator()(double number) const {
        return number;
    }
};

/// Rejects implicit conversion despite a syntactically available template specialization.
int main() {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.op1<bool, double>(fixed_generic{});
}
