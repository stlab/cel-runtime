// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>

int main() {
    cel::raw_segment program;
    cel::raw_stack stack;
    cel::frame scope(stack);
    program.execute(scope);
}
