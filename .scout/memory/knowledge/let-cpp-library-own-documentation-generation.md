---
title: "Let cpp-library own documentation generation"
entity_type: observation
confidence: 0.80
created: 2026-11-19T22:23:56Z
last_accessed: 2026-11-19T22:23:56Z
last_reinforced: 2026-11-19T22:23:56Z
access_count: 1
sources:
  - "CMakeLists.txt:18-29"
tags: ["cmake", "cpp-library", "cpp-library-setup", "documentation", "docs-target", "post-build"]
tier: knowledge
source: inferred
---

# Let cpp-library own documentation generation

Configure documentation through cpp_library_setup in CMakeLists.txt:18-29: DOCS_INPUTS selects the inputs, DOCS_EXCLUDE_SYMBOLS hides implementation details, and DOCS_OPTIONS selects README.md as the main page. The current integration has no manual documentation-copy hook.

Do not attach an add_custom_command(TARGET docs POST_BUILD ...) merely because if(TARGET docs) succeeds. In a consuming parent project, docs may belong to another directory; CMake's TARGET form requires the target to be defined in the current directory. An obsolete copy hook caused this embedding failure (https://github.com/stlab/cel-runtime/issues/9); its removal is documented in https://github.com/stlab/cel-runtime/pull/10. Prefer cpp-library's documentation settings over redundant copying. Additional customizations remain possible when their target ownership and necessity are established.
