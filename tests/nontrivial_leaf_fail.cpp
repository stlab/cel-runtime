// SPDX-License-Identifier: BSL-1.0
#include <cel/runtime/value_type.hpp>
#include <string>

/// Verifies that owning library adapters do not admit opaque nontrivial leaves.
int main() {
    const auto invalid = cel::value_type::leaf<std::string>();
    (void)invalid;
}
