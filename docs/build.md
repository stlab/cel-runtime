# Build and link the runtime with CPM {#runtime_build}

This tutorial builds `cel-runtime` from source as a static library inside
your application's CMake build. No installation or package prefix is needed.

## Before you begin

Use CMake 3.24 or later, Ninja, Git, and a C++20 compiler. The first
configuration downloads the pinned sources and build infrastructure. You need
Git read access to the dependency repositories. The runtime requires
an IEEE binary64 `double`; its headers check the representation.
The shell commands below use a POSIX shell.

## Add CPM to your application

1. Create an application directory:

   ```sh
   mkdir runtime-client
   cd runtime-client
   ```

2. Create a directory for the CPM bootstrap:

   ```sh
   mkdir -p cmake
   ```

3. Save this bootstrap as `cmake/CPM.cmake`:

   ```cmake
   file(DOWNLOAD
       https://github.com/cpm-cmake/CPM.cmake/releases/download/v0.43.2/CPM.cmake
       "${CMAKE_BINARY_DIR}/cmake/CPM_0.43.2.cmake"
       EXPECTED_HASH
           SHA256=49a3bef91ceb65bb66d57255e12d1ffd22abc2f6408fa9fe4534c544a2f232aa
       TLS_VERIFY ON
   )
   include("${CMAKE_BINARY_DIR}/cmake/CPM_0.43.2.cmake")
   ```

   CMake downloads and verifies CPM before executing it. A checksum mismatch
   stops configuration. Keep the version and checksum together when updating.

## Build the runtime with your application

Create `main.cpp` in your application directory:

```cpp
#include <cel/runtime.hpp>

int main() {
    cel::dyn_segment builder;
    builder.just(cel::value::from(5.0));
    auto program = std::move(builder).finish().prepare<double>();
    cel::raw_stack stack;
    return program.call(stack) == 5.0 ? 0 : 1;
}
```

Create `CMakeLists.txt` beside it:

```cmake
cmake_minimum_required(VERSION 3.24)
project(runtime_client LANGUAGES CXX)
set(CPM_USE_LOCAL_PACKAGES OFF)
include(cmake/CPM.cmake)
CPMAddPackage(
    NAME cel-runtime
    GITHUB_REPOSITORY stlab/cel-runtime
    GIT_TAG 8e3b08f8da2fb3a7add02ed3cc9d5bc54c606c49
    OPTIONS "BUILD_SHARED_LIBS OFF" "BUILD_TESTING OFF" "CEL_INSTALL OFF"
)
add_executable(client main.cpp)
target_link_libraries(client PRIVATE cel::runtime)
enable_testing()
add_test(NAME client COMMAND client)
```

Configure, build, and run the test from your application directory:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

Confirm that `client` passes. CPM fetches the pinned runtime source and builds
it as a static library. `BUILD_SHARED_LIBS OFF` selects static linkage,
`BUILD_TESTING OFF` disables dependency tests, and `CEL_INSTALL OFF` disables
the runtime's package installation rules. `CPM_USE_LOCAL_PACKAGES OFF` keeps
CPM from preferring an installed package.

The `cel::runtime` target supplies the headers, static library, and C++20
requirement. Do not add dependency include or library directories manually.
Keep a reviewed commit in `GIT_TAG` when updating the dependency.

Continue with [Compile and reuse a program](get-started.md) to pass arguments,
reserve stack storage, and evaluate the same program more than once.
