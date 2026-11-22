---
title: "Preserve cpp-library public and private compilation boundaries"
entity_type: decision
confidence: 0.70
created: 2026-11-22T00:58:13Z
last_accessed: 2026-11-22T00:58:13Z
last_reinforced: 2026-11-22T00:58:13Z
access_count: 1
sources:
  - "CMakeLists.txt:6"
  - "CMakeLists.txt:18-29"
  - "src/storage.cpp:89-95"
  - "stlab/cpp-library@v7.0.0:cpp-library.cmake:119-137"
tags: ["cmake", "cpp-library", "compilation-boundary"]
tier: knowledge
---

# Preserve cpp-library public and private compilation boundaries

cel-runtime relies on cpp-library to enforce the public/private testing boundary. Keep the toolkit version canonical in CMakeLists.txt:6 and register production sources and public TESTS through cpp_library_setup (CMakeLists.txt:18-29).

The cpp-library 7.0.0 migration deliberately removed the client-side private-include stripping loop: standalone public tests, including compile-fail targets, now link the production library with its public usage requirements rather than a private testing implementation. Do not restore target link/include rewriting to recreate the old topology. Private implementation tests belong in implementation translation units under CPP_LIBRARY_TESTING(), as illustrated by src/storage.cpp:89-95.

Upstream mechanism verified in stlab/cpp-library at v7.0.0, cpp-library.cmake:119-137: both compile-fail and ordinary executable targets link library_target. Preserve this distinction when adding tests or upgrading the build toolkit; public tests must not require private implementation include paths or private testing configuration.
