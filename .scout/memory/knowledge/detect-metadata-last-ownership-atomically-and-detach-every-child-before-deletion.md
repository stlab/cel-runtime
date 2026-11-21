---
title: "Detect metadata last ownership atomically and detach every child before deletion"
entity_type: decision
confidence: 1.00
created: 2026-11-19T22:25:51Z
last_accessed: 2026-11-19T22:25:51Z
last_reinforced: 2026-11-19T22:25:51Z
access_count: 1
sources:
  - "include/cel/runtime/detail/retirement.hpp:14-80"
  - "src/value_type.cpp:28-33"
  - "src/value_type.cpp:68-81"
  - "src/value_type.cpp:577"
  - "src/value_type.cpp:609"
  - "tests/resource_test.cpp:52"
tags: ["ownership", "retirement", "concurrency", "metadata"]
tier: knowledge
source: written
---

# Detect metadata last ownership atomically and detach every child before deletion

Preserve allocation-free, constant-stack metadata destruction under concurrent releases through distinct retained handles. Select the last owner with the reference count's atomic decrement (`fetch_sub(..., memory_order_acq_rel) == 1`), never with a `use_count` snapshot followed by reset: another handle can release between those operations. Only the last owner may link the dying node into a local retirement queue. Keep published metadata immutable.

Every concrete disposal callback must detach ALL runtime-owned child edges into the supplied queue before deleting its node. For `type_plan`, this includes field/parameter descriptors, the optional array/range element, and the optional callable result. Leaving even one child to ordinary recursive member destruction defeats the stack bound. Reuse dying nodes' embedded links rather than allocating traversal storage. These ownership operations do not synchronize mutable client callbacks.

Regression evidence for concurrent last-owner disposal counts and callback depth is in `src/value_type.cpp:577`; deferred queue draining is in `src/value_type.cpp:609`. These guarded in-source cases run in `runtime_in_source_tests`. Concurrent root/interior-alias releases preserving shared descendants remain in `tests/resource_test.cpp:52`, run by `resource_test`. Configure with `cmake --preset test` before building if needed, build with `cmake --build --preset test --target runtime_in_source_tests resource_test`, then run `ctest --preset test -R '^(runtime_in_source_tests|resource_test)$'`. Scheduling stress alone is not a deterministic proof of the historical race; preserve the atomic-selection and complete-detachment invariants as well as the tests.
