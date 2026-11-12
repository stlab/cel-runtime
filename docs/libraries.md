# Add types and operations {#runtime_libraries}

This tutorial extends a checked runtime program with a C++ operation and a
client value type. Build the runtime and link `cel::runtime` as described in
[Build and link the runtime with CPM](build.md).

## Add a typed operation

An operation consumes the newest operands in source order and returns one
value. The template arguments name the result first:

```cpp
#include <cel/runtime.hpp>

cel::dyn_segment builder;
builder.push_argument(0, cel::native_type<double>());
builder.op<double, double>([](double x) { return x * 2.0; });
auto program = std::move(builder).finish().prepare<double, double>();
cel::raw_stack stack;
double result = program.call(stack, 3.0); // 6.0
```

The builder checks operand types before publishing the program. The callback
must accept the declared argument types and return the declared result type;
implicit conversions do not substitute for matching types. A callback runs
during evaluation, not during construction.

Store these builder operations in your own installer functions to reuse them
across programs. The runtime does not assign language names to operations.
Use the parser's registry when your application needs named CEL functions.

## Introduce a client type

A trivially copyable C++ type can cross a checked boundary as an opaque leaf:

```cpp
struct distance {
    double metres;
};

cel::dyn_segment builder;
builder.push_argument(0, cel::native_type<distance>());
builder.op<double, distance>([](distance x) { return x.metres; });
auto program = std::move(builder).finish().prepare<double, distance>();
cel::raw_stack stack;
double result = program.call(stack, distance{12.0}); // 12.0
```

Type identity is the C++ type, not its size or display name. A leaf owns its
stored bytes, not resources reached through pointers inside those bytes.
Use the provided `std::vector<T>` and `std::tuple<T...>` adapters for owning
arrays and tuples instead of wrapping nontrivial containers as opaque leaves.

## Choose the result boundary

Use `prepare<R, Args...>()` when the application knows the signature.
Use `compiled_segment::execute` for an owning `cel::value` result when the
shape is determined at runtime. Extract a concrete result with `as<T>()`, or
inspect an aggregate with `at()` and `field()`. Aggregate copies own
independent mutable payloads; callable values share frozen executable code.

Callback exceptions propagate and evaluation retires its temporary suffix.
Caller-owned stack prefixes survive, but callback side effects are not rolled
back. Captures stored by value remain with the program; referenced objects
must outlive every call that uses them.

Use these snippets in the `main.cpp` from the CPM build tutorial. Return zero
when the result matches the expected value, then rebuild and run your client:

```sh
cmake --build build
ctest --test-dir build --output-on-failure
```

Confirm that the typed-operation example returns `6.0` and the client-type
example returns `12.0`. See
[Choose execution and ownership boundaries](execution.md) for reservation,
callable results, and structural emission.
