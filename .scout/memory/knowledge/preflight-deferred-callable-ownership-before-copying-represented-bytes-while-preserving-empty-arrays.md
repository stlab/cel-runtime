---
title: "Preflight deferred callable ownership before copying represented bytes while preserving empty arrays"
entity_type: decision
confidence: 1.00
created: 2026-11-19T22:26:06Z
last_accessed: 2026-11-19T22:26:06Z
last_reinforced: 2026-11-19T22:26:06Z
access_count: 1
sources:
  - "src/value_ops.hpp:54-73"
  - "src/value.cpp:182-246"
  - "tests/value_test.cpp:723"
  - "tests/value_test.cpp:798"
tags: ["cloning", "deferred-callables", "arrays", "exception-safety"]
tier: knowledge
source: written
---

# Preflight deferred callable ownership before copying represented bytes while preserving empty arrays

When cloning raw canonical values, reject callable values lacking installed clone/destruction operations before reading that callable's representation. This is not a promise to avoid all structural source reads: array carriers and counts must remain readable so legal empty arrays of deferred-callable element type can be preserved, including nested empty arrays.

Preflight fixed callable carriers before bulk copying fixed payload bytes. At each nonempty array boundary, use the REQUESTED element descriptor to check direct callable ownership before copying any packed element bytes; do not substitute the source array owner's installed descriptor, which could bypass the requested type's rejection. Recurse through array structure iteratively as needed. Empty arrays do not require ownership operations for an element value that does not exist.

On failure, release every initialized sibling/prefix and leave the destination uninitialized. Preserve the before-read, nested-array, partial-cleanup, and empty/nested-empty cases in `tests/value_test.cpp`. Do not replace these behavioral checks with type-only rejection of every descriptor containing a deferred callable.
