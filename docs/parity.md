# Maintain the runtime parity ledger {#runtime_parity}

This ledger helps maintainers map supported runtime contracts to C++ tests and
Rust evidence. It is not a language-parity claim or an operation census.
The Rust baseline is
`03d658f8b12acabe1d6a2503281e7c86cf00b6a3`. Rust links pin that revision;
C++ source links identify checkout tests by name or line. Update the links and
revision when changing a contract, then run its linked tests.

## Implemented and tested contracts

The following entries cover the retained runtime surfaces:

| Contract | C++ contract tests | Rust evidence | Status |
| --- | --- | --- | --- |
| Raw aligned stack; reverse owner retirement; byte/depth checkpoints | [storage tests](../tests/storage_test.cpp), [raw execution tests](../tests/raw_execution_test.cpp), including “empty frames and mixed leaves preserve stack order and cursor” | [raw_stack.rs:24–76][rust-stack] | Implemented; C++ canonical-only storage adaptation |
| Raw operation sequence; retained real captures; source-order dispatch; error cleanup | [raw execution tests](../tests/raw_execution_test.cpp), including “canonical instruction captures retain descriptors and clean failed clone suffixes” | [raw_segment.rs:68–90,393–433][rust-raw] | Implemented; C++ exception transport |
| Dynamic checked construction; preflight before effects; single-result freeze | [segment tests](../tests/segment_test.cpp), “checked signatures reject errors before callback effects” and “dynamic ordered mixed operations freeze reads and literals” | [dyn_segment.rs:757–789][rust-dyn], [prepared_segment.rs:16–75][rust-prepared] | Implemented |
| Static stack effects and declared initial operands | [segment tests](../tests/segment_test.cpp): “static initial operands retain source order and type flow”; six [compile-fail fixtures](../CMakeLists.txt) | [segment.rs:57–73,90–122,188–239,350–415][rust-typed] | Implemented; borrowed initial-input adaptation |
| Complete identity, layout, and separately checked ownership capabilities | [value tests](../tests/value_test.cpp), including “private callable ownership installation preserves shape and validates operation pairs”; [resource tests](../tests/resource_test.cpp) for shared descriptor DAGs | [value_type.rs:1–9,47–74][rust-types], [prepared_segment.rs:16–75][rust-prepared] | Implemented |
| Code-independent canonical ownership binding for complete immutable type DAGs, including empty callable composites and higher-order result witnesses | [callable tests](../tests/callable_test.cpp): ownership binding, deep/shared DAGs, foreign-operation rejection, allocation-failure cleanup; [installed consumer](../tests/installed_consumer/main.cpp) | C++ semantic-descriptor/canonical-carrier adaptation; no same-named Rust API claim | Implemented; semantic Fn declarations remain unbound |
| Recursive independent aggregate copies, native boundary adapters, over-aligned client roots | [segment tests](../tests/segment_test.cpp): “typed composite calls borrow inputs and return disjoint nested results”, “valid over-aligned client root arguments and results are admitted”; [value tests](../tests/value_test.cpp) | [prepared_segment.rs:96–225][rust-prepared] | Implemented; native C++ facades differ |
| Tuple packing at exact origin; source-order ownership transfer | [structural tests](../tests/composite_execution_test.cpp): “nested packing consumes source order and preserves exact owners” | [dyn_segment.rs:792–825][rust-dyn] | Implemented; origin includes logical depth |
| Typed arrays, empty arrays, zero-byte elements, nested owning payloads | [structural tests](../tests/composite_execution_test.cpp) packing and invalid-shape cases; [value tests](../tests/value_test.cpp) | [dyn_segment.rs:852–916][rust-dyn] | Implemented |
| Record packing and source-to-schema permutation validation | [structural tests](../tests/composite_execution_test.cpp): “record schema permutation leaves expression effects source ordered”, invalid shapes/permutations, duplicate/wrong-length rejection | [registered_ops.rs:41–90,176–255][rust-record] | Implemented runtime emitter; Rust emitter is parser-side |
| Consuming tuple projection; sibling retirement without selected-child cloning | [structural tests](../tests/composite_execution_test.cpp): “consuming projection transfers selected owners and retires siblings without cloning” | [dyn_segment.rs:931–955][rust-dyn] | Implemented/tested |
| Consuming canonical record projection; sibling retirement without selected-child cloning | [structural tests](../tests/composite_execution_test.cpp): “consuming projection transfers selected owners and retires siblings without cloning” | [lib.rs:1809–1826][rust-member-parser], [registered_ops.rs:414–435][rust-member-emitter], [geometry.rs:48–58,160–178][rust-native-fields] | Implemented/tested; canonical carrier/interface adaptation of registered field selection |
| Consuming array indexing; safe f64 rejection and prefix restoration | [structural tests](../tests/composite_execution_test.cpp): “invalid floating indices reject safely and restore prefixes” | [dyn_segment.rs:958–985][rust-dyn] | Implemented; deliberate f64 index adaptation |
| Bounded, inclusive, from, to, to-inclusive, full ranges; endpoint order and complete form identity | [structural tests](../tests/composite_execution_test.cpp): “every range kind retains endpoint metadata and source order”; [value tests](../tests/value_test.cpp) | [op_table.rs:929–1018,1525–1583,1623–1643][rust-ranges] | All six implemented; no floating iteration law |
| Dependent origins; lazy conditional/and/or; exact result join; merged reads and selected effects | [structural tests](../tests/composite_execution_test.cpp), including lazy read/preflight and logical-or cases | [dyn_segment.rs:189–229,1149–1162,1460–1509][rust-dyn] | Implemented; byte/depth origin adaptation |
| Independent closure bodies; explicit unused parameters; full preflight; unrestricted complete results | [callable tests](../tests/callable_test.cpp): “callable full declarations include unused composite parameters”, client-leaf/nested/callable result cases | [dyn_closure.rs:1–4,438–486,526–547][rust-closure] | Implemented |
| Reference-semantic carriers; same-carrier exclusion through acquisition/cleanup; distinct nesting | [callable tests](../tests/callable_test.cpp), public and dynamic acquisition reentry regressions and distinct-carrier nesting | [callable.rs:14–18][rust-callable] | Implemented; explicit per-invocation C++ scratch |
| Stable source-ordered guarded dynamic invocation across nested stack growth | [callable tests](../tests/callable_test.cpp), nested-growth reread and throwing dynamic-body tests | [dyn_segment.rs:1367–1457][rust-dyn] | Implemented; stable C++ canonical staging |
| Typed/raw preparation; no repeated descriptor comparison; caller-owned raw result retirement | [segment tests](../tests/segment_test.cpp), [installed consumer](../tests/installed_consumer/main.cpp) for public typed execution | [prepared_segment.rs:16–168,228–255][rust-prepared] | Implemented; raw result retirement tested in segment suite |
| Intermediate alignment, prefix totals, zero-byte depth, selected/known-callee peaks | [segment tests](../tests/segment_test.cpp), [structural tests](../tests/composite_execution_test.cpp), [callable tests](../tests/callable_test.cpp) | [raw_segment.rs:117–130][rust-raw] gives alignment/frame metadata | Implemented/tested; published C++ reservation helper is an adaptation, not claimed Rust API parity |
| Exception suffix cleanup, preserved prefixes, construction rollback, partial-owner retirement, reusable stack | [raw execution tests](../tests/raw_execution_test.cpp), [segment tests](../tests/segment_test.cpp), structural/callable allocation-failure and rollback cases | [raw_stack.rs:67–76][rust-stack], [raw_segment.rs:411–433][rust-raw], [dyn_segment.rs:1367–1457][rust-dyn] | Implemented |
| Measured ordinary/lazy scalar prepared zero-allocation paths; dynamic callable staging costs | [segment tests](../tests/segment_test.cpp), [structural tests](../tests/composite_execution_test.cpp), [callable tests](../tests/callable_test.cpp) allocation probes | [dyn_segment.rs:2100–2149][rust-dyn] for ordinary prepared measurements | Implemented/tested; four warmed dynamic scalar-call allocations, not an allocation-free dynamic call claim |
| Installed umbrella, standalone scalar/composite/callable clients, runnable cached example | [installed consumer](../tests/installed_consumer/main.cpp), [example](../examples/getting_started.cpp), `installed_consumer` CTest | C++ packaging adaptation; no Rust package-equivalence claim | Implemented |

## Deliberate scope and mechanism adaptations

The approved C++ runtime preserves retained ownership and execution contracts
with these boundaries:

- Built-in scalar availability is bool/f64. The parser must type both integer-
  and decimal-spelled unsuffixed literals as f64, accept `f64`, and diagnose
  unsupported numeric suffixes. These lexical rules are not runtime features.
- C++ exceptions replace Rust result-shaped runtime failure transport.
- Trivially copyable client leaves and private canonical owning headers replace
  arbitrary Rust native operand relocation. Native vectors/tuples remain
  boundary adapters; the runtime does not adopt their raw parts.
- Explicit frame/context inputs replace Rust read-opcode thread-local argument
  state. C++ convenience calls borrow inputs on caller-owned reusable storage.
- Callable copies share code and entry state. Mutable aggregates instead copy
  independently. No implicit captured CEL environment is added.
- f64 indexing uses representability checks before casting and exact integer
  bounds before access. Ranges retain metadata without inventing iteration.

## Named record field selection {#runtime_record_selection}

Rust named field access follows the registered member-operation path:

1. [`Parser::is_postfix_expression`][rust-member-parser] reads the identifier
   after `.` and calls `context.apply_member`.
2. [`DynSegmentContext::apply_member`][rust-member-context] computes
   receiver-plus-explicit-argument arity.
3. [Context lookup][rust-member-context-lookup] delegates to `lookup_scoped`.
4. [Scoped member dispatch][rust-member-dispatch] selects operation-only lookup,
   rather than ordinary callable-name dispatch.
5. [Operation lookup][rust-member-lookup] searches eligible scopes in LIFO
   order, then registered signatures.
6. [The registered emitter][rust-member-emitter] matches arity and complete
   operand types, emits the operation, and annotates its complete result.

[Native geometry field registrations][rust-native-fields] supply concrete
by-value receiver callbacks for `x`, `y`, `origin`, `size`, `min`, and `max`.
The helper at `cel-euclid/src/geometry.rs:48–58` binds exact operand/result
descriptors and emits `s.op1(op)`; registrations appear at lines 160–178.
[Record schema registrations][rust-native-records] provide Point2D, Vector2D,
and Size2D schemas.

C++ `project_record(name)` provides generic consuming selection on the canonical
record carrier. It adapts this field-selection interface; Rust does not expose
a same-named generic primitive. The native geometry callbacks do not establish
a generic Rust recursive sibling-retirement routine or identical host layout.
C++ ownership guarantees come from its canonical contract and structural tests.

## Unfinished integration

The runtime does not contain a parser, registry, overload resolution, scope
precedence, registered built-ins, literal spelling rules, or source diagnostics.
The parser milestone must enumerate the full applicable built-in census,
including extension rejection, rather than infer coverage from this runtime
table. It must add parser and differential parity tests at that boundary.
No cross-language differential runner has executed in this milestone.

[rust-stack]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/raw_stack.rs#L24-L76
[rust-raw]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/raw_segment.rs#L68-L433
[rust-dyn]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/dyn_segment.rs
[rust-typed]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/segment.rs
[rust-types]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/value_type.rs#L1-L74
[rust-prepared]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/prepared_segment.rs
[rust-record]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/registered_ops.rs
[rust-ranges]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/op_table.rs
[rust-closure]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/dyn_closure.rs
[rust-callable]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-runtime/src/callable.rs#L14-L18
[rust-member-parser]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/lib.rs#L1809-L1826
[rust-member-context]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/parser_context.rs#L846-L858
[rust-member-context-lookup]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/parser_context.rs#L743-L763
[rust-member-dispatch]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/op_table.rs#L2057-L2095
[rust-member-lookup]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/op_table.rs#L2030-L2051
[rust-member-emitter]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-parser/src/registered_ops.rs#L414-L435
[rust-native-fields]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-euclid/src/geometry.rs#L48-L178
[rust-native-records]: https://github.com/stlab/cel-rs/blob/03d658f8b12acabe1d6a2503281e7c86cf00b6a3/cel-euclid/src/records.rs#L78-L80
