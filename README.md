# CEL runtime

This C++20 library provides checked byte layouts for the CEL runtime.
The aligned-storage implementation is private. This foundation does not
include values, descriptors, segments, or a parser.

## Use byte layouts

Include the public header and link to `cel::runtime`:

```cpp
#include <cel/runtime/layout.hpp>

const cel::layout leaf{sizeof(double), alignof(double)};
const auto next = cel::align_offset(1, leaf.alignment);
```

`align_offset` rejects zero and non-power-of-two alignments with
`std::invalid_argument`. It throws `std::overflow_error` when rounding
exceeds `std::size_t`.

The private move-only storage owns aligned bytes rather than payload objects.
`reserve` preserves the initialized prefix and never narrows alignment.
`resize` initializes newly exposed bytes to zero. Reallocation transfers
bytes, so payloads must support bytewise transfer and must not depend on their
previous address. The storage does not destroy payload objects.

## Build and test

Install CMake 3.24 or later, Ninja, and a C++20 compiler. The first configure
downloads the pinned cpp-library infrastructure and doctest.
Run the generated test workflow from the repository root:

```sh
cmake --preset=test
cmake --build --preset=test
ctest --preset=test
```

CTest reports all tests passing.

## Install

Build and install the Release library:

```sh
cmake --preset=default
cmake --build --preset=default
cmake --install build/default --prefix build/install-root
```

Consumers use `find_package(cel-runtime CONFIG REQUIRED)` and
`target_link_libraries(TARGET PRIVATE cel::runtime)`.
Replace `TARGET` with your CMake target name. The private storage header
is not installed.
