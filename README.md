# CEL runtime

This C++20 library gives runtime clients complete type descriptors and
independently owned values with checked canonical byte layouts, reusable
operand stacks, structural and lazy checked builders, and first-class callable
bodies. Aligned storage is private. The library does not include operation
lookup or a parser.

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
Signature descriptors alone do not install callable ownership or bodies;
`callable` installs the complete carrier operations when it binds code.
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
operations above its frame checkpoint. The checked builder establishes
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

## Build checked and prepared programs

Use the dynamic builder to declare positional reads and exact stack effects:

```cpp
#include <cel/runtime/dyn_segment.hpp>

cel::dyn_segment builder;
builder.push_argument(0, cel::value_type::leaf<double>());
builder.just(cel::value::from(3.0));
builder.op2<double, double, double>(
    [](double older, double newer) { return older - newer; });
auto program = std::move(builder).finish();

cel::raw_stack stack;
auto bound = program.prepare<double, double>();
const auto needed = bound.requirements().for_prefix(stack.cursor(), stack.depth());
stack.reserve(needed.bytes.size, needed.bytes.alignment);
stack.reserve_depth(needed.slots);
const double result = bound.call(stack, 8.0); // 5.0
```

`op<R, Args...>` is the arbitrary-arity primitive. Dynamic `op1<A, R>`,
`op2<A, B, R>`, and `op3<A, B, C, R>` list operands before the result.
Ordinary callbacks receive only their older-first operands, as const
callbacks; the raw interface separately supplies frame-aware operations.
Named callback parameters match the declared operands after cv/reference
removal, including `const &` and `const & noexcept` call operators.
Generic callbacks expose a specialization whose parameter identities exactly
match the declared operands. Ambiguous or uninspectable overload sets require
`with_signature<Return(Parameters...)>(callback)`, which selects an actual exact
const member rather than accepting a converting invocation. Include actual
parameter and return reference qualifiers in that signature. Exact function
pointers can instead be supplied directly with an explicit cast.
Callbacks return the declared result or a reference to it, with
independent result ownership. No implicit operand/result type conversions
authorize another CEL signature. Callback exceptions replace Result-shaped
operation error transport.

Construction rejects operand underflow, complete type/ownership mismatch,
and conflicting declarations of a positional read. `finish()` rejects more
than one final operand; an empty stack produces source unit.
`execute(stack, values)` checks every emitted read before any callback effect.
`argument_reads()` and `result_type()` expose the frozen boundary requirements.
Extra unused inputs are permitted and are not materialized.

`prepare<R, Args...>()` binds complete recursive adapters once. Typed calls
borrow scalar addresses and clone only emitted reads. Unused composite inputs
are not converted. Scalar dispatch on a warmed, sufficiently reserved stack
allocates no public value boxes or descriptors and compares no signatures.
Tuple/vector inputs and results use their trusted canonical adapters and may
allocate independent owning representations. Root signatures admit valid
client leaves, including over-aligned leaves; callable-parameter restrictions
do not apply to ordinary root programs.

`requirements()` on compiled, typed-prepared, and raw-prepared handles borrows
the same frozen canonical frame requirements: maximum intermediate `alignment`,
peak zero-origin `frame_extent`, and peak `live_slots`. These include
intermediates hidden by the final result and zero-byte operands that still need
slot metadata. They are simultaneous peaks, not cumulative allocation totals.

Before entering a frame, call
`requirements().for_prefix(stack.cursor(), stack.depth())` without intervening
stack mutation. The returned `bytes` layout and `slots` count include the
caller prefix. The helper rounds the current cursor up to the program's maximum
alignment before adding its extent, conservatively covering alignment padding,
and checks both extent and depth sums for overflow. Pass these totals to
`reserve` and `reserve_depth`; reservation retains any higher existing alignment.
Construction checks intermediate layout arithmetic before publishing opcodes.
Composite payload allocations, concrete callback temporaries, result buffers,
and arbitrary callback or nested external stack work are not statically inferred.
Structural branches contribute their maximum selected demand. Direct
`push_closure`/`invoke_closure` pairs also account for the known callee body;
callable signatures obtained through values or projections cannot describe
external body costs.

## Emit structural values and lazy bodies

Use `snapshot()` before source expressions to retain their ambient byte cursor
and live-slot depth. Packing consumes the source-ordered suffix and transfers
canonical ownership, without cloning children:

```cpp
cel::dyn_segment builder;
const auto ambient = builder.snapshot();
builder.just(cel::value::from(std::vector{2.0, 3.0}));
builder.just(cel::value::from(true));
builder.make_tuple(2, ambient);
builder.project_tuple(0);
builder.just(cel::value::from(1.0));
builder.index_array();
auto program = std::move(builder).finish();
cel::raw_stack stack;
const auto result = program.execute(stack, {}).as<double>(); // 3.0
```

`make_array(count, element_type, ambient)` supports zero-byte elements and
empty typed arrays. `make_record(schema, permutation, ambient)` takes a
source-position to schema-position bijection; it changes storage positions,
not expression evaluation order. The overload without a permutation expects
schema-ordered operands. Wrong complete types, counts, permutations, and
origins throw `std::invalid_argument` before opcode publication.
`project_tuple`, `project_record`, and `index_array` transfer the selected
owner and retire siblings. Index operands are f64; nonfinite, negative,
fractional, unrepresentable, and out-of-bounds indices throw
`std::out_of_range` before unsafe conversion. `make_range(kind)` retains all
six complete f64 range forms and source endpoint order; full ranges consume
no endpoints. Ranges do not specify floating iteration.

Construct branch fragments at the origin below the condition:

```cpp
cel::dyn_segment builder;
builder.just(cel::value::from(true));
cel::dyn_segment yes(builder.before_top()), no(builder.before_top());
yes.just(cel::value::from(2.0));
no.just(cel::value::from(3.0));
builder.if_else(std::move(yes), std::move(no));
auto program = std::move(builder).finish();
```

Both fragments must produce exactly one complete, ownership-compatible
result. Only the selected fragment executes. All emitted reads from both
fragments participate in the parent's external preflight, including reads in
unselected bodies. `logical_and(rhs)` and `logical_or(rhs)` use the same
origin/read rules and short-circuit bool operands. `finish()` retains dependent
origins; `execute`, preparation, and callable construction reject a nonzero
byte **or slot** origin. A zero-byte prefix does not make a dependent body
independent.

Structural execution allocates canonical outer staging, and arrays allocate
one packed owner and element extent. It never relocates native vectors/tuples
or allocates one public owner per scalar. Required storage is acquired before
the first child transfer; initialized guards release partial ownership on
failure. Final stack publication transfers one owner without allocation.

## Receive and invoke callables

Include the installed public callable header to bind an independent body and
its full explicit parameter declarations:

```cpp
#include <cel/runtime/callable.hpp>

cel::dyn_segment body;
body.push_argument(0, cel::value_type::leaf<double>());
body.op1<double, double>([](double n) { return n + 1.0; });
cel::callable fn(std::move(body).finish(),
                 std::array{cel::value_type::leaf<double>()});
cel::raw_stack stack;
auto received = cel::callable::from_value(fn.to_value());
const auto result =
    received.call(stack, std::array{cel::value::from(4.0)}).as<double>(); // 5.0
```

Unused declared parameters remain in the callable signature. Checked calls
validate arity and every parameter's complete shape and ownership before body
effects, unlike ordinary compiled programs' read-subset validation. Parameters
admit bool/f64 and their composites. Results admit every complete owned type,
including client leaves, nested aggregates, and installed callable carriers.
`to_value()` and `from_value()` let clients receive and invoke closure results
without private headers.

Callable copies are explicit reference-semantic handles: they share frozen
code, explicit callback references, and a recursive-entry guard. They do not
claim independent copies of mutable callback state. No implicit lexical
environment exists. Exclusive access is required; recursive calls through the
same carrier or its aliases throw `std::logic_error`. Distinct carriers can
nest. Argument tables, temporary owners, and results belong to each invocation.

`push_closure(body, declarations)` emits a carrier. `invoke_closure(arity)`
expects the callee below its source-ordered arguments, validates the complete
signature during construction, and transfers all consumed owners into one
stable guarded canonical frame before user code. Nested execution can grow the
main stack without invalidating those argument views or an outer owning prefix.
Dynamic invocation enters the shared same-carrier guard before allocating
staging and keeps it through result publication and guarded cleanup.
Success transfers one independent result; exceptions retire all staged owners
and preserve their original context and the caller prefix.

Public checked calls allocate an argument-view table and result outer storage.
Dynamic invocation also allocates packed staging, a pointer table, and result
storage; scalar arguments have no individual public boxes. Warmed ordinary and
lazy scalar prepared dispatch remains allocation-free on reserved storage.
These staging costs are separate from scalar opcode dispatch costs.

All program and prepared handles are move-only. Prepared factories share a
deliberately reference-semantic frozen engine, retaining captures and metadata
after the builder or compiled handle disappears. This sharing does not promise
independent mutable capture copies or thread safety. Callbacks may reference
external caller-owned state. No callback may mutate an alias of borrowed
inputs during a call. Exceptions retire the execution suffix without allocating
cleanup storage and preserve the caller's prefix.

The static builder seeds its declared initial arguments as physical owned reads:

```cpp
#include <cel/runtime/segment.hpp>

cel::segment<cel::type_list<double, double>> builder;
auto difference = std::move(builder).op2<double>(
    [](double older, double newer) { return older - newer; });
auto program = std::move(difference).finish();
auto bound = program.prepare<double, double, double>();
cel::raw_stack stack;
const auto needed = bound.requirements().for_prefix(stack.cursor(), stack.depth());
stack.reserve(needed.bytes.size, needed.bytes.alignment);
stack.reserve_depth(needed.slots);
const double result = bound.call(stack, 10.0, 2.0); // 8.0
```

Static `op0<R>` pushes; `op1<R>` replaces the top; binary and ternary
operations infer the current suffix types. `op<R, Operands...>` consumes an
exact explicitly declared suffix. `R = void` consumes without pushing;
an empty final stack becomes `std::tuple<>`. Finalization is unavailable for
multiple remaining operands. Template ascription cannot erase initial operands
or construct a dependent stack snapshot.

## Transfer trusted canonical results

Use `prepare_raw(argument_types, result_type)` for runtime-bound record,
range, or installed callable-carrier identities without one static facade.
Preparation checks complete shape and ownership plans, including recursively
installed opaque operations.

`call_into(stack, inputs, destination)` is a raw safety boundary, not a
conversion or validation interface. Supply prepared arity and live, aligned,
exact canonical extents for every emitted read. Descriptors and input bytes
outlive the call and must not alias mutated stack or result storage. Unused
slots may contain source-unit placeholders. Reserve uninitialized, aligned,
disjoint outer storage with the frozen result's byte extent. Null represents
any zero-byte canonical layout, not only source unit.

After success, you own the initialized result. Call
`destroy_result(destination)` on that same live binding exactly once to
retire its ownership without freeing your outer buffer. You can then reuse
or free the buffer. Never destroy a failed call's uninitialized destination,
an already retired result, or a byte-transferred source. Call errors leave
the destination uninitialized and retire the execution suffix. The frozen
raw call performs no signature comparisons, conversions, or public owner
boxing; composite cloning and callback work retain their documented costs.

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
