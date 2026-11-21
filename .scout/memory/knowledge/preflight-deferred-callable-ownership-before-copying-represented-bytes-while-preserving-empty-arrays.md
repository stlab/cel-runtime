---
title: "Preflight deferred callable ownership before copying represented bytes while preserving empty arrays"
entity_type: decision
confidence: 1.00
created: 2026-11-19T22:26:06Z
last_accessed: 2026-11-19T22:26:06Z
last_reinforced: 2026-11-19T22:26:06Z
access_count: 1
sources:
  - "src/value_ops.hpp:54-70"
  - "src/value.cpp:183-282"
  - "src/value.cpp:936"
  - "src/value.cpp:946"
  - "src/value.cpp:978"
  - "src/value.cpp:1000"
  - "src/value.cpp:1021"
tags: ["cloning", "deferred-callables", "arrays", "exception-safety"]
tier: knowledge
source: written
---

# Preflight deferred callable ownership before copying represented bytes while preserving empty arrays

When cloning raw canonical values, reject callable values lacking installed clone/destruction operations before reading that callable's representation. This is not a promise to avoid all structural source reads: array carriers and counts must remain readable so legal empty arrays of deferred-callable element type can be preserved, including nested empty arrays.

Preflight fixed callable carriers before bulk copying fixed payload bytes. At each nonempty array boundary, use the REQUESTED element descriptor to check direct callable ownership before copying any packed element bytes; do not substitute the source array owner's installed descriptor, which could bypass the requested type's rejection. Recurse through array structure iteratively as needed. Empty arrays do not require ownership operations for an element value that does not exist.

On failure, release every initialized sibling/prefix and leave the destination uninitialized. Preserve the fixed-aggregate before-read case in `src/value.cpp:936`, the packed-array before-read case at `:946`, nested-array rejection at `:978`, initialized-prefix cleanup at `:1000`, and empty/nested-empty arrays at `:1021`. These guarded in-source cases run in `runtime_in_source_tests`. Configure with `cmake --preset test`, build with `cmake --build --preset test --target runtime_in_source_tests`, then run `ctest --preset test -R '^runtime_in_source_tests$'`. Do not replace these behavioral checks with type-only rejection of every descriptor containing a deferred callable.
