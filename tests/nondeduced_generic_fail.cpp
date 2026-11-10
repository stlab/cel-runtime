// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/dyn_segment.hpp>

/// Defaults its non-deduced operand to double under ordinary invocation.
struct nondeduced {
    /// Returns the operand from the ordinarily selected default signature.
    template <class T = double>
    double operator()(std::type_identity_t<T> number) const {
        return number;
    }
};

/// Rejects an explicit specialization that does not prove the ordinary invocation signature.
int main() {
    cel::dyn_segment builder;
    builder.just(cel::value::from(true));
    builder.op1<double, bool>(nondeduced{});
}
