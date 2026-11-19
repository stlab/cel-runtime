---
title: "Keep Scout knowledge filenames discoverable"
entity_type: discovery
confidence: 0.80
created: 2026-11-19T22:35:52Z
last_accessed: 2026-11-19T22:35:52Z
last_reinforced: 2026-11-19T22:35:52Z
access_count: 1
sources:
  - ".scout/memory/knowledge/portable-private-cpp-thread-linkage.md:20-24"
  - ".scout/memory/index.md:1-10"
relationships:
  - type: applies_to
    target: "Keep resource_test thread linkage portable and private"
tags: ["scout", "memory", "search", "file-classification", "filenames"]
tier: knowledge
source: inferred
---

# Keep Scout knowledge filenames discoverable

The portable-thread-linkage entry deliberately uses `portable-private-cpp-thread-linkage.md`, rather than the title-derived `keep-resource-test-thread-linkage-portable-and-private.md`. Preserve that filename when updating the entry, and keep its link in `.scout/memory/index.md` synchronized.

The former filename was classified as a test file by Scout: a scoped keyword lookup found it only with `include_tests=true`, and default memory search omitted it even for its exact title. Renaming the file, without changing its title or content, made the same exact-title and standard-C++-threads queries return it first. This was file filtering, not weak semantic content. Check classification before rewriting a missing memory or changing relevance settings; for this bank, avoid a test-like filename for knowledge entries.

This explains the naming exception for [[knowledge/portable-private-cpp-thread-linkage.md|Keep resource_test thread linkage portable and private]]. The observations describe the Scout behavior verified for this repository; they are not a claim that every filename containing the word `test` is always excluded.
