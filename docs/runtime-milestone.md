# Continue the runtime milestone {#runtime_milestone}

This handoff helps maintainers integrate a parser with the accepted C++20
runtime. It records supported surfaces and validation boundaries, not complete
language parity. The milestone date is 2026-11-06.

## Supported package surfaces

Install package `cel-runtime`, include `<cel/runtime.hpp>`, and link
`cel::runtime`. Public values, complete descriptors, raw stacks/frames, raw
operation sequences, checked dynamic builders, static stack builders,
prepared typed/raw calls, and owning callable handles are installed.
Canonical storage, ownership plans, frame access, and already-entered dispatch
bridges remain private implementation details.

The checked callable entry spans acquisition through guarded cleanup.
Semantic declarations remain distinct from runtime ownership-capable witnesses. See
[Choose execution and ownership boundaries](execution.md) for caller
contracts and [the parity ledger](parity.md) for test/source evidence and
deliberate Rust differences.

Rust named fields use registered exact unary/member operations, with native
by-value geometry callbacks as concrete examples. C++ canonical
`project_record(name)` adapts that interface; it is not a same-named Rust
primitive or a claim of identical host layout. See
[Named record field selection](@ref runtime_record_selection) for the
revision-bound dispatch/emitter evidence and its ownership limits.

## Integrate parser emission

The parser integration must preserve these runtime requirements:

1. Retain `snapshot()` before field/element expressions and pass its complete
   byte/depth origin into packing.
2. Evaluate record expressions in source order and supply source-to-schema
   permutations instead of reordering evaluation.
3. Build lazy bodies at `before_top()` below the condition; retain dependent
   origins and exact one-result joins.
4. Supply independent closure bodies and full explicit parameter declarations,
   including unused parameters. Keep results general.
5. Emit the callee below source-ordered arguments and use `invoke_closure`.
   Do not borrow main-stack argument pointers across nested growth.
6. Preserve six range forms and safe f64 indexing without adding a floating
   iteration rule.
7. Treat external body costs as unknown unless the body is directly known;
   reserve additional caller capacity before entry.
8. Inspect `operand_types(arity)` before selecting an operation overload.
   Its borrowed suffix contains local logical operands in older-first order,
   including zero-byte unit values, and excludes ambient origins. Zero arity
   and underflow return empty views. Inspection does not allocate or emit code.
   Mutation, move, or destruction invalidates the view; copied descriptors
   retain their immutable metadata independently.
9. Use `value_type::compatible_with` for complete descriptor matching.
   It checks semantic shape before stored opaque ownership operations,
   preserves shared-DAG comparison costs, and propagates allocation failures.
   `same_shape` remains a semantic comparison. Compatibility alone does not
   prove installed ownership or initialized borrowed canonical data.
10. Use `bind_runtime_ownership(type)` for admitted typed empty composites whose
    complete semantic descriptors contain deferred callables. It binds the
    full immutable DAG, including nested callable results, without bodies or
    owner effects and preserves layout, identities, and parameter admission.
    It rejects foreign installed operations. Keep `value_type::callable`
    semantic and unbound; do not manufacture dummy functions or bypass checked
    ownership admission for empty values.

The runtime does not implement registry lookup, temporary/persistent scope
ordering, registered built-in overloads, lexical adapters, grammar, or source
diagnostics. The parser's operation census and contract-derived parser tests
must cover those surfaces. Rust proc-macro token production is not a C++ text
lexer. Parser P4 adds public differential execution and error/span comparisons,
with explicit group/EOF and approved type/syntax adaptations. Its feature
handoff preserves the distinction between shared comparisons and runtime's
independent ownership, prefix, allocation-rollback, and reentry contracts.
Independent final port review and remote/platform validation remain pending.

## Reproduce local validation

The pinned cpp-library revision is
`b0b096964a93561c7dd1586fa7b0efdf5f077aab`. Its presets, doctest/CTest,
compile-fail gates, Doxygen, exports, installation, and generated CI remain in
use. The installed-consumer CTest installs the built configuration to an
isolated prefix, rejects leaked source/sibling paths, and compiles two real
clients using only the imported package.

Run Debug and lint checks from the checkout:

```sh
cmake --preset test
cmake --build --preset test
ctest --preset test
cmake --preset clang-tidy
cmake --build --preset clang-tidy
ctest --preset clang-tidy
```

Run optimized tests and strict reference documentation:

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTING=ON
cmake --build build/release
ctest --test-dir build/release --output-on-failure
cmake --preset docs
cmake --build --preset docs
```

On the verified macOS AppleClang platform, run supported AddressSanitizer and
UndefinedBehaviorSanitizer checks:

```sh
cmake -S . -B build/sanitized -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=ON \
  -DCMAKE_CXX_FLAGS="-fsanitize=address,undefined -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address,undefined"
cmake --build build/sanitized
ASAN_OPTIONS=detect_leaks=0 UBSAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build/sanitized --output-on-failure
```

All 15 CTests, including six compile-fail gates, the runnable example, and
installed consumption, pass in Debug, Release, supported ASan+UBSan, and
clang-tidy configurations. Compiler output is warning-free, formatting passes,
and strict Doxygen passes. Explicit owner counters supplement sanitizer
cleanup checks. macOS LeakSanitizer is unsupported and disabled; it is not a
passed gate. GCC, MSVC, other platforms, and remote generated CI have not run
locally. No hosting location is selected, and nothing has been published.
