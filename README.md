# CEL runtime

This C++20 library gives runtime clients complete type descriptors and
independently owned values with checked canonical byte layouts.
Aligned storage is private. The library does not include stack execution,
callable bodies, segments, or a parser.

## Construct and copy values

Include the owning-value header and link to `cel::runtime`:

```cpp
#include <cel/runtime/value.hpp>

auto original = cel::value::from(std::vector<double>{1.0, 2.0});
auto copy = original;
copy.set(0, cel::value::from(3.0));
// original.at(0).as<double>() remains 1.0.
```

`from` and `as` support bool, binary64 double, trivially copyable client
leaves, and recursively nested standard vectors and tuples. An empty tuple
represents source unit. Client-leaf registration rejects `std::string` and
other nontrivial opaque representations at compile time. Opaque leaves own
their bytes, not resources reached through client pointers. No byte-copy or
trivial-copyability test implies semantic equality.

To construct library-defined records, provide a nominal identity and named
field descriptors:

```cpp
const auto items = cel::value::from(std::vector<double>{1.0});
const std::array fields{cel::record_field{"items", items.type()}};
const auto type = cel::value_type::record("sample", fields);
auto record = cel::value::record(type, std::array{items});
auto changed = record.field("items");
changed.set(0, cel::value::from(5.0));
record.set_field("items", changed);
```

Field access and concrete extraction return independent copies. `set` and
`set_field` replace complete children of the exact expected shape; they do
not expose shared mutable references. Wrong extraction types, malformed
records, and mismatched child shapes throw `std::invalid_argument`.
Out-of-range child indices throw `std::out_of_range`. Failed construction
destroys its initialized prefix; failed replacement leaves the value unchanged.

Descriptors retain complete arrays, tuples, nominal records, all six range
forms, and callable signatures. `same_shape` compares concrete leaf identity,
complete nested shapes, record identities/names, range forms, and canonical
layouts. Equal layouts alone never authorize concrete extraction.
Callable parameters contain bool/double leaves and their composites; results
can have any valid shape, including client leaves and callable signatures.
Descriptors retain signatures without constructing callable bodies.
`owns_values()` reports complete type-level clone/destruction capabilities;
an empty array can still be owned when its element capability is deferred.

Floating-point extraction preserves IEEE binary64 values, including NaN.
The library does not expose value equality or ordering.

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
