# CEL runtime

This C++20 library gives runtime clients complete type descriptors and
independently owned values with checked canonical byte layouts, reusable
operand stacks, and stored operation sequences.
Aligned storage is private. The library does not include checked compilation,
callable bodies, control-flow emission, or a parser.

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

Arrays own one aligned allocation of canonically laid-out elements, including
tuple and record fields. Scalar elements do not have individual heap boxes.
Standard vector conversion copies elements into this allocation rather than
adopting the vector's implementation-specific storage.

Descriptor release, raw cloning, and value destruction do not recurse with
runtime nesting depth. Final cleanup does not allocate: dying metadata and
array nodes supply their own worklist links. Callable parameter eligibility
is cached in each immutable plan, and comparisons memoize distinct plan pairs
instead of expanding shared tuple graphs. No structural depth limit is imposed.
Ownership traversal selects canonical carrier slots through cached subtree
counts; its time depends on the field paths visited, not only the number of
carrier bytes.

Floating-point extraction preserves IEEE binary64 values, including NaN.
The library does not expose value equality or ordering.

## Execute stored operations

Build a trusted operation sequence and run it in an explicit caller-owned frame:

```cpp
#include <cel/runtime/raw_segment.hpp>

cel::raw_segment program;
program.push_op0<double>([](cel::frame&) { return 12.0; });
program.push_op0<double>([](cel::frame&) { return 3.0; });
program.push_op2<double, double, double>(
    [](cel::frame&, double a, double b) { return a / b; });

cel::raw_stack stack;
stack.reserve(1024, 256);
{
    cel::frame scope(stack);
    program.execute(scope);
    const double result = scope.pop<double>(); // 4.0
}
```

`push_op<R, Args...>` supports arbitrary arity; `push_op0` through `push_op3`
are wrappers. Callbacks receive the explicit frame followed by operands in
source order. They run through const typed captures, which the sequence owns
in a stable standard arena. Capture destructors run before arena release.
`R = void` produces no operand; `std::tuple<>` produces source unit.
Referenced external callback state remains borrowed and is not made thread-safe.

The raw program requires matching complete operand signatures and ownership
operations above its frame checkpoint. A later checked builder can establish
those invariants once. Safe stack/frame `push` and `pop` reject invalid
accesses; concrete `pop` also checks the complete type, not merely its byte size.
Only the innermost lexical frame mutates the stack. Direct stack mutation and
movement while a frame is active throw `std::logic_error`. The stack and
argument sources outlive all frames that borrow them.

Canonical construction, concrete operand acquisition, and payload retirement
hold a nonallocating stack-owned phase gate. Reentrant mutation, extraction,
execution, and new frame entry during these phases throw `std::logic_error`.
Clone and destruction hooks must not mutate the same stack; noexcept destruction
hooks catch a rejected request if they attempt one. The gate restores access
after exceptions and ends before the ordinary operation callback, so callbacks
can still push operands and evaluate nested frames.

Frame destruction and execution failure retire only the acquired suffix,
preserving the checkpoint prefix. A nested frame uses the same stack with its
own checkpoint and explicitly borrowed arguments/context; no thread-local
evaluation state exists. `arguments()` borrows public input owners, while
`canonical_arguments()` borrows pre-bound descriptor/pointer slots. Construct
nested frames with the input view used by their caller. `push_argument(index)`
clones only an emitted read, directly into canonical stack bytes; unused inputs
are not copied. Canonical input bytes and descriptors must remain live and must
not alias mutable evaluation storage.
Null canonical input bytes represent any zero-byte layout, including non-unit
aggregates such as a tuple of two unit fields; the complete descriptor still
distinguishes their shapes.

`reserve(bytes, alignment)` reserves payload storage and at least `bytes` active
slots. Use `reserve_depth(count)` for additional zero-byte unit operands.
Preparation initializes scalar metadata and owns captures; reserved scalar
execution and frame cleanup do not allocate. Packed composite construction or
cloning can allocate owned element storage. `push_take` and `pop_value`
transfer canonical ownership across public value boundaries without
byte-relocating the facades; public result extraction acquires outer storage.
The private checked-builder bridge can instead transfer results directly into
pre-reserved canonical result bytes without allocation or descriptor comparison.

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
