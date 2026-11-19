---
title: "Keep resource_test thread linkage portable and private"
entity_type: observation
confidence: 0.80
created: 2026-11-19T22:24:03Z
last_accessed: 2026-11-19T22:24:03Z
last_reinforced: 2026-11-19T22:24:03Z
access_count: 1
sources:
  - "CMakeLists.txt:31-38"
  - "tests/resource_test.cpp:23-36"
tags: ["cmake", "resource-test", "threads", "threads-threads", "std-jthread", "std-latch", "pthread"]
tier: knowledge
source: inferred
relationships:
  - type: relates_to
    target: "Keep Scout knowledge filenames discoverable"
---

# Keep resource_test thread linkage portable and private

Within PROJECT_IS_TOP_LEVEL AND BUILD_TESTING, CMakeLists.txt:31-38 finds Threads and links resource_test PRIVATE to Threads::Threads. The run_concurrently helper in tests/resource_test.cpp:23-36 creates two std::jthread workers behind a std::latch start gate.

Preserve portable, target-scoped thread linkage while this test uses these facilities. Standard C++ thread APIs do not remove platform compiler/linker requirements such as -pthread; CMake's FindThreads imported target encapsulates those requirements, even when it expands to no additional flags on macOS or another platform. A successful build on such a platform is not evidence that the dependency is redundant. Keep this a private test requirement, not a public dependency for every consumer. Future cpp-library centralization is compatible with this lesson if it supplies the same portable target-scoped requirement.
