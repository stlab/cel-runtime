// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime.hpp>

int main() { return cel::align_offset(1, 8) == 8 ? 0 : 1; }
