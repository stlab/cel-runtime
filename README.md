# CEL runtime

> **Note:** This project is a temporary solution for using the CEL language from
> [cel-rs](https://github.com/stlab/cel-rs) in C++. This library will be deprecated
> when using the Rust implementations from C++ becomes viable.

Build a checked program once, then evaluate it repeatedly on your own reusable
stack. This C++20 library supplies program builders, owning results, complete
value types, and callable values. To compile CEL text instead of constructing
operations in C++, start with [CEL parser](https://stlab.github.io/cel-parser/).

## Build from source with CPM

Use CPM to build a static runtime with your application. After
[adding CPM to your project](docs/build.md), declare the dependency and link
your application target:

```cmake
set(CPM_USE_LOCAL_PACKAGES OFF)
include(cmake/CPM.cmake)
CPMAddPackage("gh:stlab/cel-runtime@1.0.0")
add_executable(client main.cpp)
target_link_libraries(client PRIVATE cel::runtime)
```

Include `<cel/runtime.hpp>` in your application. The
[build tutorial](docs/build.md) includes prerequisites, a complete application,
and configure/build commands. No install step is needed.

## Build your first program

Push two owned values, add a checked operation, and prepare the result:

```cpp
cel::dyn_segment builder;
builder.just(cel::value::from(2.0));
builder.just(cel::value::from(3.0));
builder.op2<double, double, double>(
    [](double left, double right) { return left + right; });
auto program = std::move(builder).finish().prepare<double>();
cel::raw_stack stack;
double result = program.call(stack); // 5.0
```

The template arguments list the result first, followed by the operands in
source order. Preparing checks the C++ boundary types once. You can call the
prepared program again using the same stack.

## Follow the tutorials

Read these guides in order:

1. [Build and link the runtime with CPM](docs/build.md).
2. [Compile and reuse a program](docs/get-started.md).
3. [Add types and operations](docs/libraries.md).
4. [Choose execution and ownership boundaries](docs/execution.md).

The generated documentation places grouped APIs under **Reference** and
symbol lookup under **Index**. Build it with `cmake --preset docs` followed by
`cmake --build --preset docs`, then open `build/docs/html/index.html`.
