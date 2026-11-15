# 0026: `Array<T>` as a library struct

- Status: accepted (2026-11-15, revision 3, by the user "for now", including the `-O0` gate amendment of Section 4;
  revisited if the switch's measurement contradicts it)
- Spec: [§2.6](../../specs/02-types.md#26-generics), §2.6.2 and §2.6.4 (the members),
  [§3.1.1](../../specs/03-declarations.md#311-top-level-constants), [§4.3](../../specs/04-memory.md),
  [§19](../../specs/19-c-interop.md), [§20.12.5](../../specs/20-protocols.md) and
  [§21.2.2](../../specs/21-concurrency.md).
- Plan: [0011](../plan/0011-library-containers.md), Phase 3. Builds on [0025](0025-library-collection-capabilities.md).

## Context

0025 gave the compiler the capabilities a library array needs and tested them with a throwaway `Buffer<T>`. This
record decides how the real `Array<T>` is written, what is removed, and in what order. The current state, from an
inventory of the code (2026-11-15):

- **Runtime.** `lib/Runtime/core/arrays.joyeer` already implements the array algorithms in Joyeer behind the
  public C ABI `joyeer_array_*_abi` (create-owned, clone, append, remove-last, consume-empty, destroy) and the
  native `[String]`/`[UInt8]` buffer helpers. An array is a 24-byte handle `{data, count, capacity}`; `data`
  points just past a 24-byte header `{elementSize, cloneElement, destroyElement}`, and one allocation holds the
  header and `capacity` elements, so the block is `24 + capacity * elementSize` bytes. Growth is 0 to 4 then
  doubling and destruction runs the element callback from the last element to the first. `[]` still allocates a
  header (`makeArray` with count 0); the zero handle `{0, 0, 0}` is produced only by a move out of a slot and by an
  uninitialized binding, and the runtime traps on appending to it, cloning it or removing from it.
- **Compiler.** `TypeKind::array` has about 40 uses (type checker, IR verifier, lowering, backend). The
  array-specific IR opcodes are `array_append`, `array_remove_last`, `array_consume_empty`, `construct_array` and the
  array cases of `count`, `subscript` and `subscript_address`. Constant arrays are encoded by `ir/constants.cpp` and
  emitted by the backend as read-only blocks with the same header and `capacity == count`. The byte-pointer
  descriptors of `intrinsics.h` (`address_of`) name `TypeKind::array`; `native_host.cpp` and `native_functions.cpp`
  marshal native `[UInt8]`, `[String]` and `main(args:)` by element kind; `String.utf8()` calls
  `joyeer_array_create_owned_abi` directly. The `for-in` loop is a parser rewrite to `count` and `a[i]`, but the
  checker's `for-in` and `not-iterable` tests also name the type kind.
- **Cost.** `a[i]` and `count` are inline today; `append`, `removeLast` and `consumeEmpty` are calls into the
  runtime. The baseline of `tests/perf/array_*.joyeer` is recorded in the plan 0011 log.

Phase 3 must remove the type kind, the opcodes and the array cases of the backend without changing a program that
compiles today, its traps, or its allocation sizes, with no performance regression outside measured noise at `-O2`
and a bounded cost at `-O0` (Section 4). The few diagnostics and reports that do change are listed under
Consequences.

## What "done" means here

The algorithms are already Joyeer, in the runtime. The deliverable of this phase is therefore **removing compiler
paths that know about arrays by kind or by name, replacing them with the role mechanism**, not rewriting the
algorithms. After the switch the compiler keeps this accepted residual set of array-aware code, each tied to the
role and never to a spelling:

1. the role itself (shape validation, ownership, subscript typing, the `count` exception) from 0025;
2. constant blocks: the header encoding, the constant evaluator's folding of `count`, and the constant branches of
   lowering, keyed on the structure's role;
3. native marshalling of `[UInt8]` and `[String]` parameters and results, and `main(args:)`, keyed on the role and
   the element type;
4. `String.utf8()` (see Decision 2) and the byte-pointer descriptors, keyed on the role;
5. the display of the role type as `[T]` and of syntactic `Array<X>` as `[X]` by name (`platform.cpp`), the same
   display special case 0024 kept for `Optional`;
6. the `alwaysinline` attribute of the backend, keyed on the `elementAddressor` mark of the role's addressor, not
   on arrays.

`docs/impl/` states this set. The plan's wording "implement their existing operations in Joyeer" is reworded
accordingly when the plan is next edited.

## Options considered

### A. Where the algorithms live

1. **Keep the builtin array.** No work, but it ends the plan.
2. **Reimplement the algorithms generically in the library** (a per-`T` `append`, `removeLast`, growth and clone
   over `size_of<T>`, `move_in` and `move_out`). Fully library code, but each element type instantiates a copy of the
   growth and clone logic, the runtime copy stays for the C ABI, and `append` loses its single precompiled
   implementation. A regression risk with no user-visible gain.
3. **A thin typed library over the existing runtime ABI.** `Array<T>` is a struct that owns the handle and declares
   its members; each member calls the matching precompiled `joyeer_array_*_abi` function. Indexing and `count` are
   the only inline parts. Allocation sizes, growth, traps and the C ABI are the runtime's.

### B. Where the declaration lives

1. **The role prelude.** It is deliberately not a runtime unit (0025), so it may not declare C bindings.
2. **The concurrency prelude.** Wrong coupling: a caller-supplied concurrency prelude would remove `Array`.
3. **A third bundled prelude, `lib/Runtime/collections.joyeer`,** always loaded, runtime-authorized like the
   concurrency prelude. It will also host `Dict` in Phase 4.

### C. How an array literal is built

1. **Keep `construct_array`.** An array-specific opcode the plan wants removed.
2. **Lower a literal to ordinary operations** and a call of a method of the `array_literal` role.

## Decision

Option A3, B3 and C2. Nothing below adds source syntax.

### 1. The declaration

`collections.joyeer` declares `@compiler_role(array_type) public struct Array<T>` with the 0025 shape and the
existing API: `count` (read-only outside), `append(element:)`, `removeLast()`, `consumeEmpty()`. `[T]` and
`Array<T>` are the same type. Members:

- `deinit` calls `destroy_range<T>(address, count)`, sets `count` to 0, then calls the runtime destroy. The first call
  is the 0025 guard for a linear element (it traps `linear container dropped while not empty` and never destroys)
  and, for any other element, destroys the live elements last to first; clearing `count` first stops the runtime from
  destroying them again, so each element is destroyed once, `capacity` is untouched and the release size is right.
  The runtime's destroy of the zero handle returns, so a second destroy is a no-op.
- `array_element` checks `index < 0` and `index >= count` with one comparison per `if` (`array index out of bounds`)
  and returns `slot_address<T>(base: data, index: index)`. A live array always has a header, so `data == 0` implies
  `count == 0` and the second check covers it.
- `array_copy` calls `joyeer_array_clone_abi`; it exists only for a copyable `T`, so the header's clone callback is
  always valid when it runs.
- `container_end` (`consumeEmpty`) calls `joyeer_array_consume_empty_abi`, which traps `array not empty` before
  touching an element and leaves the handle zeroed; the compiler then destroys `self`, a no-op on `{0, 0, 0}`.
- `append` and `removeLast` spill the element to `stack_slot<T>()` and call the runtime; `removeLast` returns
  `.Some(move_out<T>(slot))` or `.None`.
- `array_literal` (a fourth method role, below).

The header written for `[]` and for a literal uses `size_of<T>`, `clone_callback<T>` and `destroy_callback<T>`. It is
identical to today's for every type on which an observable operation exists. It differs only where 0025 already
made the callback 0 and the difference is unobservable: a noncopyable `T` has no clone callback (cloning is rejected
statically), and a linear `T` has no destroy callback, so the trap of a dropped nonempty linear array reads
`linear container dropped while not empty` instead of the element's own text (a compile error in any accepted
program, so only a hand-written library or IR case reaches it).

Capacity and growth are the runtime's (0 to 4, then doubling, and `24 + capacity * elementSize` bytes). They are
**deliberately unspecified in the language**; the runtime documents them and the characterization suite pins the
allocation counter, not the policy.

### 2. The compiler

- **Binding.** `Array<T>` and `[T]` bind to the role symbol, as `T?` binds to `Optional` in 0024. A user declaration
  named `Array` takes precedence in its scope and prints as `<main>.Array`. `String.utf8()`, native results and
  `main(args:)` use the same symbol.
- **Removal (last step).** `TypeKind::array`, the builtin `Array` registration, the array branches and the opcodes
  `array_append`, `array_remove_last`, `array_consume_empty`, `construct_array` and the array cases of `count`,
  `subscript` and `subscript_address` are removed. Numeric opcode encodings are not renumbered: the slots are held
  (wire version 22 becomes 23 for the structure change and the removals).
- **IR mark.** `StructureDefinition` replaces `inertZero` with a `role` enum (`none`, `array`), as `EnumerationRole`
  does in 0024, plus the existing `copier`. The element type is the structure's first type argument, not a separate
  field. The verifier checks the arity and the shape per role.
- **The `array_literal` role** is a fourth method role of the `array_type` struct: a private `init(address: Int,
  count: Int)` (or, if an initializer cannot be an instance, a consuming method of a zero value, decided in the
  implementation without changing the contract) that wraps `joyeer_array_create_owned_abi`. It is unique, has
  exactly those labels and types, is private and returns the owning struct; a duplicate, a missing one or a bad
  shape is `invalid-prelude` like the other three. It inherits private-field access, the linear policy, the
  must-transfer rules and instance discovery (0025 lists it with the addressor, the copy method and `deinit`).
- **Literal lowering.** All elements are evaluated first, left to right, and become owned temporaries; only then
  are they moved into one contiguous stack block, with no fallible operation between, and the `array_literal`
  method is called. A failed element (`?`, a trap, a `return`) drops the earlier ones exactly as the arguments of
  an ordinary call are dropped. The block is a new IR opcode, `stack_block` (element type, constant count),
  verified runtime-only; for `Void` elements and for an empty literal nothing is allocated and the address is 0,
  which the runtime accepts when `elementSize == 0`.
- **A new layout intrinsic, `joyeer_intrinsic_place_address<T>(value: inout T): Int`.** Authorized units only
  (`type-checking.unauthorized-intrinsic` elsewhere, in user files and in `--runtime-module` units alike). It is
  accepted only as a **direct argument expression of a call**, so the call holds the `inout` projection for its
  duration and the existing exclusivity analysis rejects an overlapping read of the place in a sibling argument; a
  stored, returned or captured result is rejected. Accepted bases: a local `var`, an `inout` or `mutating` receiver
  (including `deinit`'s `self`), an owning `consuming` receiver (the `container_end` method's `self`), and a field
  path of those. Rejected: a `let`, a `borrowing` parameter, a constant, an rvalue. It lowers to `lowerAddress`
  and `pointer_to_integer`; no new opcode.
- **Constants.** `constant_value` and the backend's constant block keep the header and `capacity == count`. A
  constant is read-only and borrowed; an owned use goes through `array_copy`, so instance discovery must find the
  copy method for a constant used as a spawn input or result (§21.2.2). `place_address` of a constant is impossible
  by the `inout` rule.
- **`String.utf8()`** keeps its opcode in this phase and produces a role array of `UInt8` with the same layout. A
  later change may lower it through `array_literal` with a string-data-address intrinsic; that intrinsic does not
  exist for `String` today.
- **Instances.** 0025 discovery requires the element addressor and the copy method for every concrete `Array<X>`;
  this record adds the literal method. Whether discovery is narrowed to demanded instances is decided by the
  measurement below, before acceptance.
- **Owned element arguments (implementation clarification).** A consuming parameter declared with exactly the
  role's owned type parameter is an owned argument context, classified before substitution. It accepts an owned
  temporary or an unmarked independent copy of a copyable place; explicit `consume` moves it. Other parameters
  and receivers retain their ordinary markers. Checking, semantic analysis and lowering share the classification,
  so `append` preserves its existing API without dispatch on its name. A language-designer review approved this
  clarification before implementation.
- **Literal authorization (implementation clarification).** A validated factory is marked `arrayLiteral` in IR.
  The verifier permits the compiler's user-function packing sequence only as a closed graph: an inert-zero receiver,
  matching element type/count, one initialization of each bounded slot, and one matching factory call after packing.
  Raw addresses may not escape and the packing region contains no fallible operation. This is not authorization
  to write layout intrinsics in user source. Empty `Array<Never>` keeps its zero-size representation; generic
  representation queries use empty storage and a load of a `Never` value is unreachable.

### 3. The collections prelude

- **Loading.** A third bundled source, source IDs `max + 1` (concurrency), `+ 2` (roles), `+ 3` (collections);
  module flags runtime and prelude, not core; always loaded; not replaceable by a caller-supplied concurrency
  prelude. The driver's source-table check and `bindRolePreludeDeclarations` (which finds "the prelude that is not
  a runtime unit") are updated for three sources and tested for density.
- **Test seam.** `ModuleInput::rolePreludeContext` is replaced by a single enumerated field naming which bundled
  prelude a module replaces (none, concurrency, roles, collections). A replaced collections prelude must itself
  declare an `array_type` struct, or name resolution reports `invalid-prelude` for every program, as for the roles
  prelude; the 0025 tests (33) move their `Buffer<T>` to replace the collections prelude and keep working because
  roles bind by role, not by name.
- **Cross-prelude names.** The collections prelude refers to `Optional` and `Result` of the roles prelude, as the
  concurrency prelude already refers to `Result`; a test pins it with a caller-supplied concurrency prelude.
- **Packaging.** The embedded source needs `prelude_source.h.in`, `prelude.cpp`, `CMakeLists.txt`, the release
  staging and `docs/building.md`. Compile time is measured before and after.
- **Linkage.** An element addressor has internal linkage (stage 3), so the out-of-line copy of every inlined instance
  is discarded; before this an instance of a prelude function had external linkage and the copy survived (a size
  cost, measured above). The switch also internalizes authorized runtime wrappers and generated ownership helpers:
  calls across object boundaries use explicit C exports or callback addresses, never their private numeric names.
  The size gate checks the effect.

### 4. Cost mechanism, measurement and gates

**Mechanism.** The addressor is emitted `alwaysinline` (the backend sets the attribute on a function marked
`elementAddressor`), and the backend's `-O0` pipeline is LLVM's `buildO0DefaultPipeline`, which runs the
always-inliner. Its address is `joyeer_intrinsic_slot_address<T>(base: Int, index: Int): Int` (§20.12.5): `base +
index * size` with no overflow check, a plain GEP in the backend, because the library checks the index first. The
addressor is written with one comparison per `if`: a `||` costs a stack slot and two branches at `-O0`. Both the
attribute and the intrinsic are implemented in the measurement change (wire version 21) and have tests.
The switch also marks closed owned-element argument wrappers `alwaysInline` in verified IR, independent of method
names. This removes the additional `append` wrapper frame even at O0. Helpers keep `NoInline` when debug information
is present, but not at `-g0`, where there are no debug scopes to corrupt. These changes optimize the existing
mechanism; they do not amend any gate.

**Method.** `tests/perf/benchmark_addressor.py` builds `array_index_flat` and `array_drop_flat` with the compiler
(the builtin array) and the matching `*_library` programs with `array-addressor-probe` over
`array_buffer_prelude.joyeer`, a minimal library array that follows Decision 1: storage and header from the
runtime's create function, the two-check addressor over `slot_address`, and `deinit` destroying the range, clearing
the count and freeing the block. Two differences remain, both against the library: without `place_address` the create
function writes into a stack slot that is read back, and `deinit` frees the block itself because the backend's own
declaration of the runtime destroy function cannot be matched by a Joyeer binding. Both programs of a pair print the
same checksum, and both use the same trap text. Everything was measured on Windows x64 (an idle machine), with the
compilers of branch `feat/library-array-switch` on f9252c4 plus the measurement change, LLVM 22.1.8, one Release
compiler building both profiles, 25 interleaved repetitions per configuration (50 for the rerun and the baseline), and
the paired median ratio library/builtin with its 95% bootstrap interval.

How the addressor got there (`index`, no debug information):

| Addressor | -O0 | -O2 |
|---|---|---|
| as merged in 0025: a real call, overflow-checked arithmetic | 1.84 [1.78, 1.89] | 0.94 [0.91, 1.00] |
| plus `alwaysinline` | 1.47 [1.42, 1.49] | 0.94 [0.81, 1.06] |
| plus `slot_address` and one comparison per `if` | 1.11 [1.09, 1.13] (50 repetitions) | 0.96 [0.93, 0.99] (50 repetitions) |

The final library array (the like-for-like prelude above) by debug mode:

| Program | Debug | -O0 | -O2 |
|---|---|---|---|
| `index` | none (rerun, 50) | 1.11 [1.09, 1.13] | 0.96 [0.93, 0.99] |
| `index` | line tables | 1.12 [0.99, 1.16] | 0.97 [0.88, 1.05] |
| `index` | full | 1.11 [1.08, 1.12] | 1.00 [0.93, 1.03] |
| `drop` | none | 1.06 [1.01, 1.10] | 1.00 [0.96, 1.01] |
| `drop` | full | 1.09 [1.04, 1.13] | 1.00 [0.98, 1.04] |

The first `index`/none run was disturbed at `-O2` (medians near 900 ms against a 90 ms minimum) and was rerun once at
double the repetitions, as the rerun rule below allows. Every build with line tables and with full debug information
passed the LLVM verifier and printed the right checksum. Executables grow by 1 KiB at `-O0` and 1.5 KiB at `-O2` for
`index` and 1 KiB at both for `drop`.

**Reading.** At `-O2` the library array costs nothing measurable. At `-O0` it costs about 10%: the inlined addressor
keeps its parameter allocas and loads, and a drop adds the `NoInline` destroy helper and the library `deinit`. The
`NoInline` guards inlined debug scopes in helpers that have no `DISubprogram` and is not array-specific;
the switch restricts it to builds with debug information. Giving the helpers a scope remains a possible later change.

**Gates.** Revision 2 of this record proposed, before any measurement, a median ratio of at most 1.10 at `-O0` and
1.03 at `-O2` for `array_index`, and a lower interval bound of at most 1 for each of the five `array_*` programs; 0025
asked for no regression outside noise and a bound on per-instance code size. The measurement shows that the `-O0`
part cannot hold: 1.11 to 1.12 for `index` and 1.06 to 1.09 for `drop`. This record therefore **amends 0025's cost
gate for `-O0` only**, on the ground of the preamble (§0.1.2: parity with the builtin is a measurement goal, not a
guarantee) and of `-O0` being the debugging profile, and **asks for acceptance of the amended numbers explicitly**:

- `-O2`, every array program of the baseline and both prototype pairs: no regression outside noise (the lower bound
  of the 95% interval at most 1), and a median ratio of at most 1.03 for `index` and `drop`.
- `-O0`, every array program and both pairs, in every debug mode: a median ratio of at most **1.15** (revision 2
  proposed 1.10; the change is made after measuring and is stated as such).
- Size: each of the five `array_*` executables grows by at most 4 KiB at `-O0` and 2 KiB at `-O2`; `hello` does not
  grow at `-O2`. This is also the trigger for narrowing instance discovery to demanded instances.
- Compile time (predeclared, not yet measured): the front end of `hello` (`--emit-llvm`, which runs everything but
  the LLVM pipeline and the link) grows by at most 15%.
- Platforms: the gates apply on Windows x64; Linux x86_64 is measured and reported with the switch, and becomes gated
  once its noise is known.
- The `array_append_growth` `-O0` baseline was re-recorded with 50 repetitions (437.7 ms minimum, 450.5 ms median, a
  tight set), so it is gated like the others. Append changes most in the switch (a library method, a spill and the
  runtime call, one call level more than today) and has no prototype, because writing it needs `place_address`; it is
  the first number to read when the switch is measured.

**Rerun rule.** A configuration that fails, or whose interval is plainly disturbed, is rerun once with double the
repetitions; both runs are reported, and the rerun decides. A gate that still fails blocks the switch: it is not
renegotiated in the pull request. The change is then optimized, the general fallback below is taken, or this record
is reopened with the user's explicit approval.

**Fallback.** If a later change loses the inlining, the fallback is an IR-level inline-always expansion of any
role-marked addressor, a general mechanism. An array-specific inline check in the backend is excluded.

## Consequences

- **Compatibility.** No source change. Traps and texts (`array index out of bounds`, `array not empty`, the
  runtime's allocation overflow texts), allocation and free sizes, growth, destruction order and evaluation order are
  unchanged and pinned by the characterization suite. A trap's source location moves into the prelude and is not
  pinned, uniformly.
- **Diagnostics and reports that do change** (listed, pinned, and regenerated with the project's tool): the
  members `address` and `capacity` of an array turn from unknown member into `inaccessible-declaration`;
  `--report-instantiations` goldens gain `Array` instance lines (`tests/type-checking/generics_report.txt` and
  `generic_types_report.txt`); the IR dump text changes; `extension Array<T>: P`, which is rejected today, is pinned
  before the switch and its outcome after it is stated (the role closes extensions in the prelude, and a user
  extension is rejected as for `Optional`).
- **Recursive types.** `struct Tree { var kids: [Tree] }` and a non-indirect `enum Json { Array([Json]) }` work
  today because a builtin array is indirect. The library array has three `Int` words but "owns" its element in the
  0025 sense, so every recursive-layout and recursive-ownership check must treat the role as indirect. The
  characterization suite includes these, mutual recursion `A { [B] }` / `B { [A] }`, and a user enum case named
  `Array`.
- **Plan 0012 (Equatable).** Its todo gives the builtin array a conformance through an element compare and is built
  on the type kind. Conditional conformance is deferred (§20.10). Which lands first, and how `[Int] == [Int]` behaves
  before and after, is pinned first; the decision of 0012 is amended if it changes.
- **Runtime.** The `joyeer_array_*_abi` symbols and their C declarations stay: they are the public ABI and the
  library calls them. The runtime functions use no state beyond the allocator, the constant blocks are read-only,
  and a spawned array stays bitwise relocatable, so thread safety is unchanged.
- **Wire.** Version 23 (the structure role, the removals; 21 and 22 are taken by `slot_address` and `stack_block`);
  malformed-IR tests cover the removed opcodes, the new opcode and the role arity.
- **Dictionary.** Unchanged. `Dict` stays builtin until Phase 4; its keys and values may be arrays, which reach the
  copier and the destroy helper through the dictionary's type arguments (a test copies and drops a `[String: [Int]]`
  with no direct array operation, and takes `&d[k].append(element: 1)`).
- **Documentation.** Spec §2.6, §2.6.2, §2.6.4, §3.1.1, §19, §20.12.5, §21.2.2; `docs/impl/{supported-features, ir,
  backend, runtime, c-interop, name-resolution}.md`; `docs/building.md`; the plan log.

### Staging

The switch touches name resolution, the checker, lowering, the verifier, the wire, the backend and a new prelude,
so it is not one change. In order, each revertable:

1. **Characterization PR** (tests only, on today's builtin array; it must pass there): every array diagnostic as a
   golden; trap texts and the trap order against later arguments; destruction order of `[Resource]` with a printing
   `deinit` at scope end, on overwrite, on `?` and on early return, including a nested `[[Resource]]` whose inner
   array was emptied by `removeLast`; `removeLast` and `consumeEmpty` for copyable, noncopyable and linear
   elements, empty and nonempty; every zero-state element class of 0025 section 4; `[Void]`, `[Never]`, `[Any]` and
   `[(Int): Int]`; a literal with a fallible element, `func f(): Result<[Res], E> { return .Ok([Res(id: 1),
   make()?, Res(id: 3)]) }`, with the allocation counter at zero; constants at every use; `main(args:)` and the
   native `[String]` and `[UInt8]` parameters and results; `String.utf8()`; the recursive-type, Dict, extension and
   equality programs above; the printed type name in diagnostics.
2. **Measurement PR** (Section 4): prototype and measure the addressor and the helper `NoInline` change; fix the gate
   numbers here; re-baseline. This record is accepted only after these numbers exist.
3. **Infrastructure PR:** the third prelude loader, source IDs, the seam, packaging, `place_address`, `stack_block`
   and the `array_literal` role, tested with a `Buffer<T>` declaration in a replaced collections prelude. `Array` is
   not touched.
4. **Switch PR:** bind `[T]`, bundle the real `Array`, constants, native marshalling, wire 23; the executed
   fixtures of Phase 2 (a library array through every operation at `-O0` and `-O2` compared exactly, an array of
   `JoinHandle` through a task) close here, since they can only run once the real `Array` exists.
5. **Cleanup PR:** delete the dead opcodes and `TypeKind::array`.

### Tests (besides the characterization suite)

- **Roles and prelude:** the `array_literal` shape (unique, labels, privacy, result); a duplicate `array_type`; a
  missing role; the seam replacing collections; a caller-supplied concurrency prelude; source-ID density with three
  preludes and the driver's table check.
- **`slot_address`** (with the measurement change): rejected outside authorized units with and without type
  arguments; per-instance lowering; a plain GEP with no overflow check; `Void` and zero-size elements; the verifier's
  malformed forms; the wire round trip; the stage fixture `tests/c-interop/layout-intrinsics.joyeer`.
- **`place_address`:** the direct-argument form accepted; a stored or returned form, a `let`, a `borrowing`
  parameter, a constant and an rvalue rejected; unauthorized use rejected in a user file and in a runtime module;
  `-O2` ordering of the `count` store against the escaping call.
- **Literals:** evaluation order with side effects, `[]`, a literal of 1000 elements at `-O0` and `-O2`, an element
  that traps, a linear literal `[spawn(...)]`.
- **Zero handle:** a moved-from array in a struct field that is dropped; an uninitialized `var`; no accepted program
  clones, indexes or appends to a zero handle.
- **Cost gate:** the five `array_*` programs in both profiles, the `hello` size and the compile time, against the
  numbers fixed after the measurement.

## Open questions

1. Whether giving the ownership helpers a `DISubprogram` (so `NoInline` can go) is worth doing; only if a `-O0` gate
   needs it (Section 4).
2. Whether instance discovery is narrowed to demanded instances, if the size gate of the five `array_*` programs
   fails.
3. Whether `array_literal` is an initializer or a consuming method (a compiler detail, not a contract).
4. Whether a later change reimplements the algorithms generically (Option A2) for inlining at `-O2`; out of scope
   here, and only if the benchmarks justify it.

## Review

Revision 1 was reviewed 2026-11-15: accept with changes. Folded into revision 2: the literal's ownership on early
exit and its stack block; the `array_literal` role as a method of the struct; a fully specified `place_address`; the
cost mechanism made a precondition, with the debug-scope and `NoInline` hazard, alias information and a generic
fallback; the third prelude's seam, source IDs and packaging; an honest residual set instead of "no special case";
the structure `role`; the header claim reworded; the recursive-type, Dict, extension and equality risks; and a staged
plan of five changes. Revision 2 with the first measurement was reviewed 2026-11-15: accept with changes, not yet
accepted (gates loosened after measuring without saying so and contradicting 0025, the excluded `-O0` append result,
the `slot_address` contract, a drop prototype that was not like for like). Folded into revision 3: a like-for-like
prototype and a remeasurement, an explicit `-O0` amendment of 0025 that names the number revision 2 proposed, size and
compile-time gates, a rerun rule and a rule for a missed gate, the re-recorded append baseline, the `slot_address`
contract in §20.12.5, the residual set's `alwaysinline`, and the wire and linkage corrections. The review approves a
design, not an implementation.

## Evidence

The inventory above (2026-11-15) and the baseline of `tests/perf/array_*.joyeer` in the plan 0011 log. Prior art:
0024's role binding of syntax, 0025's capabilities, 0023's bundled-prelude separation.
