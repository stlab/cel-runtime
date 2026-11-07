// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/segment.hpp>
/// Rejects a static unary operation when no operand exists.
int main() {
    cel::segment<cel::type_list<>> builder;
    auto invalid = std::move(builder).op1<double>([](double number) { return number; });
    (void)invalid;
}
