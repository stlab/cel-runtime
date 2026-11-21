---
title: "Inline doctest diagnostics need direct ostream includes on MSVC"
entity_type: discovery
confidence: 0.99
created: 2026-11-21T11:03:48Z
last_accessed: 2026-11-21T11:03:48Z
last_reinforced: 2026-11-21T11:03:48Z
access_count: 1
sources:
  - "src/value_type.cpp:508-519"
  - "src/callable.cpp:153-158"
tags: ["testing", "doctest", "msvc", "headers", "inline-tests"]
tier: knowledge
source: Verified Windows CI failures and repairs in PR stlab/cel-runtime#11; source ranges rechecked after merge.
---

# Inline doctest diagnostics need direct ostream includes on MSVC

When moving tests into production translation units, keep a direct `<ostream>`
include inside each affected `#if CPP_LIBRARY_TESTING()` block. Inline doctest
uses declarations rather than the standalone implementation-with-main header
path. String and `string_view` `CHECK` diagnostics can instantiate standard stream
insertion while `basic_ostream` is still incomplete on MSVC; another translation
unit's include cannot fix it.

This migration first failed Windows in `value_type.cpp`, then exposed the same
independent failure in `callable.cpp`. Guarded `<ostream>` includes in both files
fixed the failures without adding stream or doctest dependencies to production
builds. Preserve these per-translation-unit includes when reorganizing private
tests; do not rely on standalone test-main transitive includes.
