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
  - "src/value_type.cpp:67-84"
  - "tests/resource_test.cpp:71"
  - "tests/resource_test.cpp:144"
tags: ["ownership", "retirement", "concurrency", "metadata"]
tier: knowledge
source: written
---

# Detect metadata last ownership atomically and detach every child before deletion

Preserve allocation-free, constant-stack metadata destruction under concurrent releases through distinct retained handles. Select the last owner with the reference count's atomic decrement (`fetch_sub(..., memory_order_acq_rel) == 1`), never with a `use_count` snapshot followed by reset: another handle can release between those operations. Only the last owner may link the dying node into a local retirement queue. Keep published metadata immutable.

Every concrete disposal callback must detach ALL runtime-owned child edges into the supplied queue before deleting its node. For `type_plan`, this includes field/parameter descriptors, the optional array/range element, and the optional callable result. Leaving even one child to ordinary recursive member destruction defeats the stack bound. Reuse dying nodes' embedded links rather than allocating traversal storage. These ownership operations do not synchronize mutable client callbacks.

Regression evidence is in `tests/resource_test.cpp`: concurrent last-owner disposal counts and callback depth, deferred queue draining, and concurrent root/interior-alias releases preserving shared descendants. Scheduling stress alone is not a deterministic proof of the historical race; preserve the atomic-selection and complete-detachment invariants as well as the tests.
