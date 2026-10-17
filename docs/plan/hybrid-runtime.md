# Hybrid Joyeer/C Runtime Plan

> **Status:** proposed; all phases below remain pending. This document plans
> implementation work, not new language rules or current support. Current
> behavior belongs in [runtime](../impl/runtime.md),
> [strings](../impl/string.md), and the
> [implemented surface](../impl/supported-features.md).

Public module names, project layout, and joypm/CMake ownership are tracked in
the [library plan](library.md). The runtime migration does not depend on
building that public library, and public source-library development can begin
before runtime migration.

## 1. Goal and First Milestone

Build one runtime with two implementation languages:

- Joyeer implements portable algorithms and, incrementally, storage policies.
- C provides a small native substrate for allocation, platform APIs, startup,
  and failure paths that must not depend on higher-level runtime operations.
- Generated programs continue linking one `JoyeerNativeRuntime` static
  archive. LLVM/LLD stay in the compiler backend, not the program runtime.

The first milestone replaces string equality, ordering, and concatenation
algorithms with Joyeer implementations. Preserve the existing string layout,
source behavior, runtime entry points, and allocation ownership. Keep string
clone/destruction, arrays, dictionaries, and host operations in C initially.
A migration unit is one operation, not necessarily an entire type.

Success means the original C algorithms no longer supply these operations in
production. Joyeer wrappers around the same C algorithms do not meet this goal.

### Non-goals

- Rewriting the C++ compiler or eliminating the system C library.
- A public general-purpose FFI, arbitrary user pointers, reference types, or
  a stable third-party library ABI.
- User generics, protocols, closures, custom destructors, or concurrency as
  prerequisites for the first milestone.
- Changing ordinary value copying to implicit moves, adding GC/ARC/COW, or
  making `String` enforce valid UTF-8.
- Shipping separate selectable `core`/`alloc`/`std` runtime profiles, new
  freestanding targets, or a package registry.
- Moving directories or introducing a separate library for every layer.

## 2. Current Constraints

| Surface | Current implementation and consequence |
|---|---|
| Runtime archive | [CMake](../../src/runtime/CMakeLists.txt) builds five C sources into one static archive; no Joyeer runtime objects are built yet. |
| Strings and containers | [runtime.c](../../src/runtime/runtime.c) owns storage and algorithms; ordinary heap-backed copies clone independent storage. |
| Compiler lowering | [String arithmetic](../../src/backend/native_functions.cpp) and [collections](../../src/backend/native_collections.cpp) call runtime functions. Implementing those functions with the same high-level operations can recurse. |
| Native boundary | [Filesystem](../../src/runtime/filesystem.h) and [process](../../src/runtime/process.h) interfaces use flat inputs, output pointers, and explicit error tags/codes. These are not source-language FFI. |
| Linkage | [Function emission](../../src/backend/native_types.cpp) and [ownership helpers](../../src/backend/native_ownership.cpp) export numeric-ID names. Separately compiled runtime and application objects can collide. |
| Build ordering | [Compiler CMake](../../src/compiler/CMakeLists.txt) makes `joyeer` depend on the runtime, adds a runtime link dependency, and copies it after building the compiler. A runtime that needs `joyeer` would introduce a cycle. |
| Language support | The [implemented surface](../impl/supported-features.md) excludes user generics, methods, custom `init`/`deinit`, general FFI, and separately linkable source libraries. |

The backend already emits native objects internally; the missing work is a
supported internal runtime-object path with explicit bindings, exports, and
an acyclic build. Existing module support alone does not provide these.

## 3. Responsibility Boundaries

```text
Application machine code / compiler-generated ownership helpers
                         |
             Existing runtime ABI entry points
                         |
              Joyeer portable implementations
                         |
          C substrate / compiler storage primitives
                         |
                 Platform APIs / system C library
```

| Responsibility | Initial owner |
|---|---|
| Allocation, reallocation, release, and allocation-balance accounting | C substrate |
| Process entry, argument conversion, exit checks, minimal panic/output | C substrate |
| Filesystem/process calls and platform encoding conversion | Existing C host implementation |
| String equality, ordering, and concatenation | Joyeer after the required gates |
| String clone/destruction, arrays, dictionaries | Existing C implementation until separate follow-up gates |
| Scope cleanup, value-copy/transfer decisions, typed ownership helpers | Compiler, unchanged in responsibility |
| Future text/path utilities and convenient host wrappers | Source standard library, not new compiler builtins |

C-to-Joyeer-to-C calls are acceptable if they descend to lower-level
capabilities. No implementation may reach its own exported operation through
an operator, implicit copy, cleanup helper, or indirect callback.

## 4. Minimum Runtime-Writing Capabilities

These are proposed internal capabilities, not accepted source syntax. Define
their signatures and diagnostics before implementing them. Do not use a
module name or source-directory location as proof that code is trusted.

### 4.1 Native bindings and ABI exports

- Add an explicit toolchain-controlled runtime compilation context and a
  narrow binding table for approved substrate functions and runtime exports.
  Ordinary programs must not acquire these bindings through imports alone.
- Support the scalar types and internal address representation required by
  the existing ABI, with explicit target-sized conversion and Boolean rules.
- Emit C-callable adapters for the existing exported signatures; keep native
  aggregate results behind output pointers rather than assuming Joyeer
  structs or enums have a C-compatible by-value calling convention.
- Internalize or uniquely namespace all non-exported runtime functions and
  ownership helpers. Export only the approved runtime interface, not
  incidental `joyeer_fn_<id>` or `joyeer_clone_type_<id>` symbols.
- Diagnose unknown bindings, duplicate exports, unsupported types, and
  incompatible signatures through the normal stable-ID diagnostics system.

Retained C convenience entry points may become thin adapters, but there must
be exactly one production algorithm provider for each migrated operation.
The interface remains private and version-locked to the compiler.

### 4.2 Storage primitives without managed-container dependencies

- Provide internal borrowed byte regions and raw result storage, distinct
  from owning `String`/`Array` values; do not implicitly clone or destroy raw
  addresses.
- Support byte access, checked offset/length calculations, and bulk copying
  through explicit compiler primitives or substrate calls. Specify readable
  and writable extents, null/zero-length behavior, initialization, and
  non-overlap requirements.
- Route allocation/release through the existing tracked allocator. Preserve
  the empty-result storage policy and panic-on-allocation-failure behavior.
- Reject invalid sizes or overflow before allocation or memory access. A
  zero-byte operation must not dereference an otherwise permitted null input.
- Keep minimal panic reporting independent of managed string formatting,
  concatenation, and heap allocation.

Byte storage is enough for the first milestone. Typed uninitialized storage,
element alignment, and copy/move/destroy operations are later container gates;
do not expose an unrestricted raw-memory API to solve this narrower task.

### 4.3 Ownership and compatibility

- Inputs remain borrowed; successful results acquire exactly one destruction
  obligation. Result storage is published only after initialization.
- Existing compiler-generated clone/take/destroy machinery stays responsible
  for source value semantics. A copied address is not an independent string.
- Joyeer-allocated string storage must remain releasable by the retained C
  destroy path, using the same allocator and accounting.
- Define cleanup for recoverable partial failures. Do not disguise a failed
  operation as an empty result or silently fall back to the old algorithm.
- Preserve arbitrary bytes, embedded NUL, byte indexing, independent copies,
  and current panic/error contracts.
- Preserve legacy `readFile` behavior and the separate filesystem/process
  error types. UTF-8 host-path validation must not become content validation.

Custom user `deinit` is not needed while existing ownership hooks remain in
charge. Later user-defined resource types require a separately completed
noncopyability/destruction design consistent with
[the memory model](../spec/04-memory.md).

### 4.4 Object generation and bootstrap

- Add an internal object-only path: explicit sources, no application `main`,
  no executable link, and no requirement for an already-built full runtime.
- Preserve verified IR, source spans, debug information, optimization options,
  and target identity. Do not turn this into a public binary-module promise.
- Separate compiler construction from product staging. Relocate the existing
  runtime dependency, `LINK_DEPENDS`, and post-build copy so runtime generation
  does not depend on a compiler target that itself waits for that runtime.
- Assemble the Joyeer and C objects into the existing static archive. Only
  then stage a usable compiler/backend/runtime product and build joypm.

```text
C++ compiler + backend -> joyeer -> runtime Joyeer objects --+
C compiler ----------------------> native substrate objects +-> runtime archive
runtime archive + compiler/backend --------------------------> product staging
staged toolchain --------------------------------------------> joypm / acceptance
```

Use the newly built compiler, not a previously installed compiler or runtime.
List runtime sources explicitly, as required by the
[module input contract](../spec/12-modules.md). No source discovery, mandatory
Python scripting, external LLVM executables, or new cross-compilation support
is implied. Keep artifact names and install components unchanged.

## 5. Implementation Phases

All phases are pending. H1 and H2 can proceed independently after H0.

| Phase | Prerequisites | Deliverables and exit criteria |
|---|---|---|
| H0: Contracts and baseline | None | Inventory runtime entry points and generated dependencies; specify the binding, storage, ownership, export, and build contracts above. Capture focused behavior/allocation baselines and failure cases before changing providers. |
| H1: Internal bindings and storage | H0 | Wire runtime-only bindings/exports and byte-storage primitives through every applicable frontend, IR/verifier, backend, and diagnostic surface. Scalar C/Joyeer round trips and byte-region tests pass; ordinary-source misuse is rejected. |
| H2: Runtime object build | H0 | Add object-only output, isolate private generated symbols, and remove the compiler/runtime target cycle. A scalar runtime export links with C from a clean build without a preinstalled Joyeer or completed runtime; missing-main checks remain unchanged for applications. |
| H3: Leaf string algorithms | H1, H2 | Replace equality and ordering with Joyeer byte algorithms. Compile out the corresponding C algorithms, retain necessary adapters, and prove allocation-free behavior, exact comparison results, and absence of recursive runtime dependencies. |
| H4: Owned string result | H3 | Replace concatenation with checked length calculation, one tracked result-buffer allocation, and bulk copies. Preserve the existing ABI/layout and retained C clone/destroy compatibility; independent-value and boundary tests pass. |
| H5: Product acceptance | H4 | Validate clean/incremental builds, installation/relocation, joypm bootstrap, Debug/Release behavior, and full native acceptance for each claimed platform/architecture. Update implementation documentation with measured evidence. |

The first milestone ends at H5. It does not require every remaining C
operation to migrate.

## 6. Validation and Acceptance

Extend the existing [tests](../../tests/) rather than adding a parallel test
framework. Use focused tests during development; final acceptance requires
default builds and unfiltered CTest with unit tests and joypm enabled, as
specified in [building](../building.md).

### Compiler and artifact gates

- Test accepted/rejected internal bindings, address/ABI types, export names,
  signatures, storage operations, and malformed IR. Runtime compilation must
  not weaken ordinary source diagnostics, visibility, or ownership checks.
- Inspect emitted IR/object definitions and references: one provider per
  migrated entry point, no application/runtime numeric-symbol collisions, no
  unexpected high-level runtime dependencies, and no recursive provider path.
- Test normal archive extraction and Windows whole-archive linking. A
  retained C algorithm must not mask a missing or unreferenced Joyeer object.
- Check runtime object dependencies, rebuild invalidation, parallel ordering,
  Debug source locations, staging freshness, and installed/relocated lookup.
- Never ship a test reference implementation or a second fallback runtime.

### String and allocation gates

- Cover empty inputs, identical values, unequal lengths, shared prefixes,
  embedded NUL, invalid UTF-8, bytes above `0x7f`, and large inputs.
  Ordering must return exactly `-1`, `0`, or `1`.
- Verify concatenated bytes and byte count exactly. Releasing either source
  must not invalidate the result; retained clone/destroy paths must work in
  nested structs, arrays, dictionaries, and enum payloads.
- Exercise the declared invalid-input, overflow, and allocation-failure
  paths without introducing unchecked memory access or success-shaped errors.
- Assert no tracked allocations for comparison and exactly one result-buffer
  allocation for concatenation, including empty results. Use focused test
  instrumentation if needed: final allocation balance alone does not measure
  allocation frequency. Require zero balance after normal cleanup.
- Keep concatenation linear in the combined byte count; do not rebuild
  prefixes through repeated single-byte strings. Record time/allocation
  comparisons against the C baseline before accepting the provider change.

### Host and product regression gates

- Run the existing runtime, file-I/O, filesystem, process, host, and joypm
  gates. Preserve Unicode path conversion, original native error codes,
  byte-exact stderr, exclusive file creation, process statuses, and argument
  boundaries.
- Preserve link semantics, packaging contents, install components, and the
  absence of LLVM/Python dependencies in generated programs.
- Record fresh native Debug/Release results with platform, architecture, and
  toolchain provenance. One host's results do not certify another platform.

## 7. Follow-up Gates, Not First-Milestone Requirements

- **Additional leaf algorithms:** migrate UTF-8 validation, lexical path
  algorithms, and numeric formatting where it reduces duplication. Keep
  Windows/POSIX encoding and path contracts explicit.
- **String lifecycle:** migrate clone/destruction only after allocator
  provenance, implicit-copy behavior, and bootstrap-safe cleanup are verified.
- **Containers:** choose user generics or a bounded internal type-operation
  mechanism before moving typed storage, growth, lookup, and destruction.
  Do not replace independent value copies with shallow pointer copies.
- **Owned OS resources:** complete custom destruction/noncopyability and
  partial-initialization behavior before exposing long-lived files or sockets.
- **Source standard library:** follow the
  [library modules and joypm plan](library.md) for the selected
  `Foundation`/`System`/`Collections`/`Testing` direction, `src/library/`
  layout, source-module management, and separate installed-SDK resolution
  gate. Public library development need not wait for runtime migration.
  Revisit methods, protocols, formatting, streaming, and other APIs separately.

Update [runtime](../impl/runtime.md), [strings](../impl/string.md),
[backend](../impl/backend.md), the
[implemented surface](../impl/supported-features.md), and relevant
[build/install documentation](../building.md) as phases actually land.
Remove completed work from this plan; proposals and planned syntax must never
be reported as implemented support.
