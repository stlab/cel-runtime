# CEL runtime

`cel-runtime` is a C++20 library for clients that build a program once and
evaluate it repeatedly on a caller-owned stack. It provides checked builders,
complete recursive types, owning values, and callable results. It does not
provide a CEL text parser, operation registry, or registered built-in library.

## Install and start

You need CMake 3.24 or later, a C++20 compiler, and Ninja for the supplied
presets. The first configuration downloads the pinned
[cpp-library infrastructure](https://github.com/stlab/cpp-library/tree/b0b096964a93561c7dd1586fa7b0efdf5f077aab).

1. From this checkout, build the library:

   ```sh
   cmake --preset default
   cmake --build --preset default
   ```

2. Install it to a prefix you own:

   ```sh
   cmake --install build/default --prefix "$PWD/build/install-root"
   ```

3. Follow [Compile and reuse a program](docs/get-started.md) to build and run
   the installed example.

Include `<cel/runtime.hpp>` and link the imported CMake target `cel::runtime`
from `find_package(cel-runtime CONFIG REQUIRED)`.

## Choose your next task

Use these pages to work with the runtime:

- [Compile and reuse a program](docs/get-started.md): run the compiled example
  against the installed package.
- [Choose execution and ownership boundaries](docs/execution.md): prepare
  calls, reserve prefixed storage, and handle owning or callable results.
- [Maintain the runtime parity ledger](docs/parity.md): find contract tests
  and source-backed Rust comparisons.
- [Continue the runtime milestone](docs/runtime-milestone.md): identify parser
  integration work and local validation limits.

To generate the API reference and these pages with Doxygen 1.13.1 or later,
run:

```sh
cmake --preset docs
cmake --build --preset docs
```

Open `build/docs/html/index.html`. Documentation warnings fail the build.
