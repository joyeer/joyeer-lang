# Hybrid Joyeer/C Runtime Plan

> **Status:** runtime migration is proposed; no production operation has moved
> to Joyeer yet. The scalar C binding and internal object-build path are
> implemented ([impl/c-interop](../impl/c-interop.md)). Current behavior:
> [runtime](../impl/runtime.md), [strings](../impl/string.md). Remove completed
> work from this plan.

## 1. Goal

One runtime, two languages: Joyeer for portable algorithms, C for the native
substrate (allocation, platform APIs, startup, panic). Programs still link one
`JoyeerRuntime` static archive.

**First milestone:** string equality, ordering, and concatenation are
implemented in Joyeer. Keep the string layout, behavior, entry points, and
allocation ownership. Clone/destroy, arrays, dictionaries, and host operations
stay in C. Wrapping the old C algorithm does not count.

**Non-goals:** rewriting the compiler, public FFI, generics/protocols/closures
as prerequisites, GC/ARC/COW, enforcing UTF-8 on `String`, selectable runtime
profiles, a package registry.

## 2. Constraints

- Runtime archive is five C sources today; no Joyeer objects yet.
- Compiler string and collection lowering calls runtime functions. A Joyeer
  implementation must not reach its own operation through an operator, implicit
  copy, or cleanup helper (recursion).
- Generated functions and ownership helpers use numeric-ID names, so separately
  compiled runtime and application objects can collide. Non-exported symbols
  must be internal or namespaced; export only the approved interface.
- Runtime generation must not depend on a compiler target that waits for the
  runtime.

## 3. Ownership of Work

| Work | Owner |
|---|---|
| Allocation, release, accounting; startup; minimal panic | C substrate |
| Filesystem/process calls | Existing C host code |
| String equality, ordering, concatenation | Joyeer (this plan) |
| String clone/destroy, arrays, dictionaries | C, until follow-up gates |
| Scope cleanup, copy/transfer decisions | Compiler (unchanged) |

## 4. Prerequisite: Storage Primitives

Needs a separate design (see [c-interop §3.2](c-interop.md)) before H3/H4:

- Borrowed byte regions and raw result storage, distinct from owning values.
- Byte access, checked offset/length math, bulk copy; define extents,
  null/zero-length behavior, initialization, non-overlap.
- Allocation through the existing tracked allocator; keep the empty-result
  policy and panic-on-failure.
- Reject invalid sizes and overflow before allocation or access.
- Panic reporting independent of managed string formatting and heap allocation.
- Inputs are borrowed; a successful result has exactly one destruction
  obligation, published only after initialization. Joyeer-allocated strings
  must be releasable by the retained C destroy path.

## 5. Build Shape

```text
compiler + backend -> joyeer -> runtime Joyeer objects --+
C compiler -----------------> substrate objects ---------+-> runtime archive
runtime archive + compiler/backend -> product staging -> joypm / acceptance
```

- Use the newly built compiler, not an installed one.
- List runtime sources explicitly; no source discovery.
- Keep artifact names and install components unchanged.
- One production provider per migrated operation. Retained C entry points may
  be thin adapters.

## 6. Phases

| Phase | Prerequisites | Deliverable and exit criteria |
|---|---|---|
| H0: Baseline | Binding/object foundation | Inventory migrated entry points and their dependencies; record behavior, allocation, and failure baselines |
| H3: Byte access, equality, ordering | H0, storage design | Borrowed byte access; Joyeer equality and ordering replace C; C algorithms compiled out; no allocation, exact `-1/0/1`, no recursive dependency |
| H4: Concatenation | H3 | Checked length, one tracked allocation, bulk copy; layout and C clone/destroy compatible; independence and boundary tests pass |
| H5: Product acceptance | H4 | Clean and incremental builds, install/relocation, joypm bootstrap, Debug/Release, native acceptance per claimed platform; evidence added to impl docs |

The milestone ends at H5.

## 7. Acceptance

Extend existing [tests](../../tests/); final gate is default builds plus
unfiltered CTest with unit tests and joypm enabled ([building](../building.md)).

**Compiler/artifact**
- Accepted/rejected bindings, export names, signatures, malformed IR; ordinary
  diagnostics and ownership checks unweakened.
- One provider per migrated entry point; no numeric-symbol collisions; no
  recursive or unexpected runtime dependencies.
- Archive extraction and Windows whole-archive linking; a retained C algorithm
  must not mask a missing Joyeer object.
- Rebuild invalidation, parallel ordering, Debug locations, staging freshness,
  relocated lookup. No test reference implementation or fallback runtime ships.

**Strings/allocation**
- Cover empty, identical, unequal length, shared prefix, embedded NUL, invalid
  UTF-8, bytes above `0x7f`, large inputs.
- Exact concatenated bytes and length; releasing either source leaves the
  result valid; clone/destroy works in structs, arrays, dictionaries, enums.
- Overflow and allocation-failure paths are checked, never success-shaped.
- No allocation for comparison; exactly one for concatenation (including
  empty); zero balance after cleanup; linear time. Compare against the C
  baseline before accepting.

**Host/product**
- Existing runtime, file-I/O, filesystem, process, host, and joypm gates pass;
  Unicode paths, native error codes, stderr bytes, exclusive create, process
  statuses preserved. Legacy `readFile` unchanged.
- No LLVM/Python dependency in generated programs; packaging unchanged.
- Fresh native Debug/Release results per platform; one host does not certify
  another.

## 8. Follow-ups (not first milestone)

- More leaf algorithms: UTF-8 validation, lexical paths, numeric formatting.
- String clone/destroy, after allocator provenance and bootstrap-safe cleanup.
- Containers, after generics or an internal type-operation mechanism.
- Owned OS resources, after custom destruction and noncopyability.
- Source standard library: see [library](library.md).

Update [runtime](../impl/runtime.md), [strings](../impl/string.md),
[backend](../impl/backend.md),
[supported features](../impl/supported-features.md), and
[building](../building.md) as phases land.
