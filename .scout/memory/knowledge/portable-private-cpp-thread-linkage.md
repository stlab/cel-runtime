---
title: "Use cpp-library default portable thread linkage"
entity_type: observation
confidence: 0.80
created: 2026-11-19T22:24:03Z
last_accessed: 2026-11-19T22:24:03Z
last_reinforced: 2026-11-19T22:24:03Z
access_count: 1
sources:
  - "CMakeLists.txt:6-7"
  - "CMakeLists.txt:18-29"
  - "docs/runtime-milestone.md:78-88"
tags: ["cmake", "resource-test", "threads", "threads-threads", "std-jthread", "std-latch", "pthread"]
tier: knowledge
source: inferred
relationships:
  - type: relates_to
    target: "Keep Scout knowledge filenames discoverable"
---

# Use cpp-library default portable thread linkage

The cpp-library 6.2.0 setup enables portable thread support by default.
`CMakeLists.txt` pins that release and calls `cpp_library_setup()` without
opting out. Its setup finds Threads and links the compiled runtime publicly
to `Threads::Threads`; generated test implementations and runners inherit
the requirement. Do not restore the former resource_test-only manual link.

The metadata and retirement concurrency tests use two `std::jthread` workers
behind a `std::latch` start gate. Public metadata coverage remains in
`tests/resource_test.cpp`; private retirement coverage lives in guarded
in-source tests in `src/value_type.cpp`.

Standard C++ thread APIs do not remove platform compiler/linker requirements
such as `-pthread`. CMake's imported target encapsulates those requirements,
even when it adds no flags on macOS. A passing build on such a platform does
not establish that the dependency is redundant.
