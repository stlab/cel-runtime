# Compile and reuse a program {#runtime_get_started}

This tutorial helps C++20 clients evaluate a cached program on reusable
storage. You construct operations directly; this runtime does not parse text.

## Before you begin

Install the library by following [Install and start](../README.md).
Run the following commands from the runtime checkout. The consumer uses the
checkout only for its application sources, never for runtime headers or
library linkage.

## Build the installed example

1. Configure the standalone consumer against the installation:

   ```sh
   cmake -S tests/installed_consumer -B build/example-consumer \
     -DCMAKE_PREFIX_PATH="$PWD/build/install-root"
   ```

2. Build the consumer:

   ```sh
   cmake --build build/example-consumer
   ```

3. Run its tests:

   ```sh
   ctest --test-dir build/example-consumer --output-on-failure
   ```

   Confirm that both `installed_getting_started` and
   `installed_runtime_consumer` pass. The second client also checks nested
   owning copies and a callable received after its builders disappear.

## Reuse the stack

The example compiles subtraction with one positional `double` argument. It
binds the complete signature once, reserves storage around an existing bool
prefix, and makes two calls. Both calls leave the prefix live.

The compiled source is `examples/getting_started.cpp`:

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

For a separate application, use the same package and target declarations:

```cmake
find_package(cel-runtime CONFIG REQUIRED)
add_executable(client main.cpp)
target_link_libraries(client PRIVATE cel::runtime)
```

The target supplies installed headers, the library, and its C++20 requirement.
For raw result storage, composite boundaries, or nested calls, see
[Choose execution and ownership boundaries](execution.md).
