# Choose execution and ownership boundaries {#runtime_execution}

This guide helps runtime clients choose checked or prepared execution and
manage stack, aggregate, and callable ownership. Use the generated API
reference for exact declarations and
[Compile and reuse a program](get-started.md) for a runnable example.

## Choose complete value types

Built-in scalars are bool and binary64 `double`. The supported platform must
satisfy the library's binary64 representation checks. Registered client leaves
must be trivially copyable, including over-aligned leaves. That requirement
does not imply semantic equality, deep immutability, or ownership of resources
reached through pointers.

Complete types distinguish leaf identity, array elements, tuple fields,
nominal record identities and named fields, range forms and endpoints, and
callable parameter/result types. Equal byte layouts or `same_shape` alone do
not authorize foreign ownership operations at an installed boundary.

Public `value` copies have independent mutable aggregate payloads.
Last-owner retirement also covers runtime-managed callable literals and lazy
executable bodies. It allocates no traversal storage and consumes no
nesting-dependent stack, including during unwinding. Shared child code stays
live until its last handle releases it. Client capture destructors retain
their own costs and behavior.
`value::from` and `as` adapt recursively nested vectors and tuples;
`at`, `field`, and concrete extraction return independent copies.
`set` and `set_field` replace exact complete children without exposing mutable
internal references. Empty arrays retain an explicit element type, and an
empty tuple represents source unit. Ordinary aggregates, including recursively
nested vectors and tuples, can contain valid trivially copyable client leaves.
Callable parameter admission is narrower; see
[Bind and receive callable results](#runtime_callable_binding).

## Choose a checked or prepared call

`dyn_segment` validates instruction composition before publication.
`op<R, Args...>` accepts arbitrary arity with exact callback parameter and
result identities after cv/reference removal. Dynamic `op1<R, A>`,
`op2<R, A, B>`, and `op3<R, A, B, C>` list the result first, followed by
source-ordered operands.
Callbacks consume older-first operands and return independently owned results.
Generic callbacks need a deduction-proven exact specialization; ambiguous
const member overloads use `with_signature<Return(Parameters...)>`.
Function pointers can select an exact overload with an explicit cast.
Exceptions provide fallible operation transport.

`finish()` requires one result or synthesizes source unit for an empty ordinary
stack. `compiled_segment::execute(stack, values)` validates every emitted read's
complete shape and ownership before effects. Extra unused inputs remain unused.
Construction rejects underflow, conflicting read declarations, and incompatible
operands. Nonzero byte **or depth** origins reject independent execution and
preparation.

Use `prepare<R, Args...>()` for typed repeated execution. Preparation binds
complete recursive adapters once. Calls borrow scalar addresses, clone only
emitted reads, and do not repeat recursive signature comparisons or create
scalar public owner boxes. Unused composites are not converted. Ordinary root
programs admit valid client leaves; closure parameter restrictions do not
narrow root signatures.

Program and prepared handles are move-only and retain reference-semantic frozen
code.

`layout` describes byte extents and alignments for reservation. Public
clients do not manage the private aligned storage implementation.

## Reserve before entering a frame

Compiled and prepared handles publish `requirements()`: maximum intermediate
alignment, peak zero-origin canonical frame extent, and peak simultaneous live
slots. These are peaks, not sums over instructions. Intermediate alignment can
exceed result alignment. Zero-byte values still occupy logical slots.

Before frame entry, compute
`requirements().for_prefix(stack.cursor(), stack.depth())`, then pass its total
`bytes.size` and `bytes.alignment` to `reserve` and its total `slots` to
`reserve_depth`. Measure the prefix without intervening mutation. The helper
rounds the byte cursor up to the frame alignment, checks byte/depth overflow,
and includes the live caller prefix. Reservation retains higher existing
alignment. Public reservation is forbidden while a frame is active.

Selected lazy branches contribute a maximum, not a sum. A directly known
callee contributes its intermediate alignment, extent, and slots.
Signature-only external callees cannot publish their body costs through a
signature. Reserve additional capacity before entry when you know those
external bodies. Composite payload allocations, client callbacks, native
adapters, argument tables, and caller result buffers are outside main-stack
requirements.

## Emit structural and lazy operations

Use `snapshot()` before evaluating source expressions. It retains their
ambient byte cursor **and** logical depth. Packing validates and consumes the
complete source-ordered suffix relative to that origin; a dependent builder
cannot consume its ambient prefix.

`make_tuple`, typed `make_array`, and `make_record` transfer canonical owners,
without cloning children. Records accept a source-position to schema-position
bijection; this permutation changes storage positions, never expression
evaluation order. Invalid counts, types, permutations, and origins reject
before publication. `project_tuple`, `project_record`, and `index_array`
consume the outer owner, transfer the selected child, and retire siblings.

Array indices are f64. Finite, nonnegative, integral, exclusive-power-of-two
representability checks precede a safe `size_t` cast. An exact integer length
comparison then precedes array access. Invalid categories throw
`std::out_of_range`; a rounded floating `SIZE_MAX` bound never admits an
unrepresentable index.

`make_range` retains all six forms: bounded, bounded inclusive, from, to,
to-inclusive, and full. Endpoint order follows source order. Full ranges
consume no endpoints. Range metadata does not define floating-point iteration.

Create `if_else` bodies at `before_top()`, immediately below the bool condition.
Both bodies need exactly one ownership-compatible complete result and the
same origin. Only the selected body executes, but both bodies' declared reads
participate in parent preflight. `logical_and` and `logical_or` short-circuit
bool bodies under the same origin/read rules.

## Bind and receive callable results {#runtime_callable_binding}

Bind an independent frozen body with explicit full parameter declarations
using `callable(body, parameters)` or `push_closure(body, parameters)`.
Parameters admit bool/f64 and their supported composites; results admit any
valid complete owned type, including client leaves and callables.
Include unused parameters; checked `call` validates every declared parameter
and arity before body effects. The body has no implicit lexical environment.
Explicit instruction captures and borrowed client callback references are
separate from CEL closure parameters. Descriptors and code outlive temporary
builder/signature objects; borrowed callback references stay caller-owned.

`to_value()` publishes an owning carrier. `callable::from_value(result)`
receives that installed carrier through public interfaces and rejects foreign
same-shape ownership ABIs. Complete compatibility also checks every callable
result's ownership, including callables inside aggregate results, before
checked execution or preparation admits the input. Prepared execution does not
repeat these comparisons. Results may be client leaves, nested aggregates, or
another callable. The installed consumer exercises receipt after builders and
the temporary compiled factory disappear.

`value_type::callable(parameters, result)` describes a semantic signature
without ownership operations. Use `bind_runtime_ownership(type)` when checked
emission needs a runtime ownership-capable descriptor without a function body,
including empty typed arrays of callables:

```cpp
const auto declared = cel::value_type::callable(
    {}, cel::value_type::leaf<double>());
const auto bound = cel::bind_runtime_ownership(declared);
cel::dyn_segment builder;
builder.make_array(0, bound, {});
cel::raw_stack stack;
auto empty = std::move(builder).finish().execute(stack, {});
// empty.size() == 0; declared.owns_values() == false.
```

Binding traverses the complete immutable metadata graph, including callable
results, arrays, tuples, and nominal records. It preserves semantic identity,
layout, names, ordering, and parameter admission; client leaves and callables
do not become valid parameters. Already canonical owners remain compatible,
and unchanged metadata stays shared. Foreign installed callable operations
anywhere in the graph throw `std::invalid_argument` rather than being replaced.
No owner operation executes.

The returned descriptor is a *capability witness*, not an initialized callable,
executable body, or permission to treat arbitrary bytes as live values.
The original declaration remains unbound, and checked emission still rejects
it. Repeated binding is representation-compatible. Binding takes expected O(V + E + S) time, O(V) auxiliary space, and at most
O(V + E + S) newly allocated result metadata for distinct metadata nodes V,
edges E, and copied name bytes S. Traversal and cleanup do not recurse.
Allocation failures propagate and leave the original graph unchanged.

Callable copies are *reference-semantic handles*: aliases share frozen code,
callback references, and entry state, not independently copied mutable
callback captures. Same-carrier entry through an alias throws
`std::logic_error`. This exclusion spans public/dynamic acquisition, staging,
dispatch, result publication, and cleanup. Distinct carriers can nest.
Invocation argument tables and result guards belong to each call.

`invoke_closure(arity)` consumes the callee below its source-ordered arguments.
Stable guarded canonical staging keeps argument bytes valid when nested
execution grows the main stack. Successful invocation transfers one independent
result; exceptions release staged owners while preserving the caller prefix.

## Handle failures and sharing

Frame destruction and exceptions retire only the evaluation suffix. The caller
retains its byte/depth prefix, and the stack remains reusable. Cleanup does not
roll back effects in client callbacks. Failed structural acquisition and
partial initialization release acquired owners; no success-shaped default
replaces a failure.

A stack permits one exclusive evaluator at a time. Scoped distinct-carrier
nested evaluation is supported, not concurrent independent access to one
stack. Canonical acquisition, publication, and retirement phases reject
reentrant mutation/extraction/frame entry. These phases end before ordinary
callbacks. Independent stacks do not make mutable captures or borrowed client
state thread-safe. Callable carriers also require exclusive access; the
same-carrier guard is not a synchronization primitive.

## Account for allocation costs

Canonical internal ownership is separate from public C++ boundary adapters.
Relocation transfers an internal logical owner and retires its source; public
value copying never relocates native objects bytewise or adopts standard
container raw parts. Aggregates copy their mutable payloads independently.

Prepared scalar execution reuses reserved stack storage. Public checked callable
calls allocate an argument-view table and result storage/facade. Structural
outer staging, array owners and element extents, composite reads/results, native
adapters, and client callbacks can allocate.

Reservation does not promise allocation-free callbacks or composite results.
Choose prepared scalar calls when you need to avoid repeated type matching
and public scalar owner construction.
