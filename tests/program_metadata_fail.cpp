// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>

int main() {
    cel::dyn_segment builder;
    auto program = std::move(builder).finish();
    return program.argument_reads().empty() ? 0 : 1;
}
