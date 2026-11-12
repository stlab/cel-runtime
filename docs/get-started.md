# Compile and reuse a program {#runtime_get_started}

This tutorial helps C++20 clients evaluate a cached program on reusable
storage. You construct operations directly; this runtime does not parse text.

## Before you begin

Create the CPM application from
[Build and link the runtime with CPM](build.md). Keep its dependency
declaration and `target_link_libraries` call.

## Build the reuse example

1. Replace the application's `add_executable` line with the example source
   fetched by CPM:

   ```cmake
   add_executable(client "${cel-runtime_SOURCE_DIR}/examples/getting_started.cpp")
   ```

2. Build the consumer:

   ```sh
   cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
   cmake --build build
   ```

3. Run its tests:

   ```sh
   ctest --test-dir build --output-on-failure --verbose
   ```

   Confirm that `client` passes and prints `5` followed by `7`.

## Reuse the stack

The example compiles subtraction with one positional `double` argument. It
binds the complete signature once, reserves storage around an existing bool
prefix, and makes two calls. Both calls leave the prefix live.

Start the builder with a positional argument, then add a constant and an
operation. The operation reads the older argument before the newer constant,
so its result is `input - 3.0`. Freezing prevents further emission, and
preparation binds the result and argument as `double`.

The complete compiled source is `examples/getting_started.cpp`:

\include getting_started.cpp

The executable prints:

```text
5
7
```

`finish()` freezes the builder. `prepare<double, double>()` binds a double
result and one double argument; `call(stack, input)` borrows the input and
returns its independent result. A prepared handle retains frozen code, so you
can discard the builder and original compiled handle.

The reservation includes the stack's measured byte cursor and logical depth.
Do not mutate the prefix between measuring it and reserving. The bool remains
owned by the caller until `pop<bool>()` retires it.

To adapt the example, copy its source into your application's `main.cpp` and
restore `add_executable(client main.cpp)`. The `cel::runtime` target continues
to supply headers, the static library, and its C++20 requirement.
For raw result storage, composite boundaries, or nested calls, see
[Choose execution and ownership boundaries](execution.md).
