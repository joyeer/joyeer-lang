# 0026: `Array<T>` as a library struct

- Status: proposed (2026-11-15)
- Spec: not yet. On acceptance §2.6 (the `Array` text), §2.6.2 and §2.6.4 (the members), §3.1.1 (constant arrays)
  and §20.12.5 (the `array_literal` role and the place-address intrinsic) change; §2.6 stops calling `Array` a
  type the compiler understands directly.
- Plan: [0011](../plan/0011-library-containers.md), Phase 3. Builds on [0025](0025-library-collection-capabilities.md).

## Context

0025 gave the compiler the capabilities a library array needs and tested them with a throwaway `Buffer<T>`. This
record decides how the real `Array<T>` is written and what is removed. The current state, from an inventory of
the code (2026-11-15):

- **Runtime.** `lib/Runtime/core/arrays.joyeer` already implements the array algorithms in Joyeer, behind the
  public C ABI `joyeer_array_*_abi` (create-owned, clone, append, remove-last, consume-empty, destroy) and the
  native `[String]`/`[UInt8]` buffer helpers. An array is a 24-byte handle `{data, count, capacity}`; `data`
  points just past a 24-byte header `{elementSize, cloneElement, destroyElement}`, and one allocation holds the
  header and `capacity` elements. The block is `24 + capacity * elementSize` bytes, growth is 0 to 4 then
  doubling, and destruction runs the element callback from the last element to the first.
- **Compiler.** `TypeKind::array` has about 40 uses (type checker, IR verifier, lowering, backend). Array-specific
  IR opcodes are `array_append`, `array_remove_last`, `array_consume_empty`, `construct_array`, and the array cases
  of `count`, `subscript` and `subscript_address`; constant arrays are encoded by `ir/constants.cpp` and emitted by
  the backend as read-only blocks with the same header and `capacity == count`. The layout intrinsic table
  (`intrinsics.h`) names `TypeKind::array` for its byte-pointer operations. The `for-in` loop is a parser rewrite
  to `count` and `a[i]`, so it needs no work of its own.
- **Cost.** `a[i]` and `count` are inline today; `append`, `removeLast` and `consumeEmpty` are calls into the
  runtime. The baseline of `tests/perf/array_*.joyeer` is recorded in the plan 0011 log.

Phase 3 must remove the type kind, the opcodes and the backend cases without changing a program that compiles
today, its diagnostics, its traps, its allocation sizes or its performance outside measured noise.

## Options considered

### A. Where the algorithms live

1. **Keep the builtin array.** No work, but it ends the plan: every container stays a compiler special case.
2. **Reimplement the algorithms generically in the library** (a per-`T` `append`, `removeLast`, growth and
   clone written over `size_of<T>`, `move_in` and `move_out`). Fully library code, but each element type
   instantiates a copy of the growth and clone logic, the runtime copy stays for the C ABI, and `append` loses its
   single precompiled implementation. A regression risk with no user-visible gain.
3. **A thin typed library over the existing runtime ABI.** `Array<T>` is a struct that owns the handle and
   declares its members; each member calls the matching precompiled `joyeer_array_*_abi` function, which is
   already Joyeer code. Indexing and `count` are the only inline parts. Allocation sizes, growth, traps and
   the C ABI are the runtime's, unchanged.

### B. Where the declaration lives

1. **The role prelude** (`roles.joyeer`). It is deliberately not a runtime unit (0025), so it may not declare C
   bindings, and the array needs them.
2. **The concurrency prelude.** Wrong coupling: a caller-supplied concurrency prelude would remove `Array`.
3. **A third bundled prelude, `lib/Runtime/collections.joyeer`,** always loaded, runtime-authorized like the
   concurrency prelude, not replaceable by a caller-supplied prelude (the reasoning of 0024).

### C. How an array literal is built

1. **Keep `construct_array`.** An array-specific opcode the plan wants removed.
2. **Lower a literal to ordinary operations:** stack storage for the elements, a `move_in` of each, then a call
   of a library function marked with a new role that wraps `joyeer_array_create_owned_abi`.

## Decision (proposed)

Option A3, B3 and C2. Nothing below adds source syntax or changes a program that compiles today.

### 1. The declaration

`collections.joyeer` declares `@compiler_role(array_type) public struct Array<T>` with the 0025 shape (private
`address`, `count`, `capacity`; the three method roles) and the existing API: `count` (read-only outside),
`append(element:)`, `removeLast()`, `consumeEmpty()`, and `[T]` as an alias of `Array<T>`. Members:

- `deinit` calls `destroy_range<T>(address, count)`, sets `count` to 0, then calls the runtime destroy. The first
  call is the 0025 guard for a linear element (it traps `linear container dropped while not empty` and never
  destroys) and, for any other element, destroys the live elements last to first; clearing `count` first stops the
  runtime from destroying them again, so each element is destroyed once.
- `array_element` bounds-checks (`array index out of bounds`, also for `data == 0`) and returns
  `data + index * size_of<T>()`.
- `array_copy` calls `joyeer_array_clone_abi`; it exists only for a copyable `T`, so the header's clone callback
  is always valid when it runs.
- `container_end` (`consumeEmpty`) calls `joyeer_array_consume_empty_abi`, which traps `array not empty` before
  touching an element and leaves the handle zeroed; the compiler then destroys `self` (a no-op on `{0,0,0}`).
- `append` and `removeLast` spill the element to `stack_slot<T>()` and call the runtime; `removeLast` returns
  `.Some(move_out<T>(slot))` or `.None`.

The header written by the library (`array_literal` and the empty array) uses `size_of<T>`, `clone_callback<T>`
and `destroy_callback<T>`, so a header is identical to today's for every element type: nothing in the C ABI,
the native `[String]` and `[UInt8]` paths, constants or the runtime changes.

### 2. The compiler

- **Binding.** `Array<T>` and `[T]` bind to the role symbol, exactly as `T?` binds to `Optional` in 0024; a user
  declaration named `Array` takes precedence in its scope and prints as `<main>.Array`. A type prints as `[T]`.
- **Removal.** `TypeKind::array`, the builtin `Array` registration and every `TypeKind::array` branch become
  role-type branches (`isArrayRoleType`) or disappear; `array_append`, `array_remove_last`,
  `array_consume_empty`, `construct_array` and the array cases of `count`, `subscript` and `subscript_address`
  are removed. `stringUtf8` and the native buffer paths stay: they produce an `Array<UInt8>` or `Array<String>`
  value of the same layout.
- **IR.** `StructureDefinition` gains the element type for a role array (`arrayElement`), so constants, the native
  parameter and result marshalling (`native_host.cpp`, `native_functions.cpp`) and the byte-pointer intrinsics
  recognize an array by that, not by a type kind.
- **A new role `array_literal`** on a library function `func name<T>(address: Int, count: Int): Array<T>` that
  wraps `joyeer_array_create_owned_abi`. Lowering a literal allocates stack storage, `move_in`s each element and
  calls it; evaluation order and ownership are those of a call whose arguments are the elements.
- **A new layout intrinsic, `joyeer_intrinsic_place_address(inout value): Int`,** the address of a place, for
  passing a receiver to a runtime function. Authorized units only; the address is valid for the call.
- **Constants.** `constant_value` and the backend's constant block keep the header and `capacity == count`; a
  constant is never destroyed, a copy goes through `array_copy`, and the clone at the spawn boundary (§21.2.2)
  is a copy. The `Array` struct definition tells the backend a constant's element type.
- **Instances.** Existing 0025 discovery requires the element addressor and the copy method for every concrete
  `Array<X>`. The cost gate below decides whether that is narrowed to demanded instances.

### 3. Cost mechanism

The addressor and `count` are the only inline parts. The addressor is emitted with `alwaysinline`; the backend
builds its `-O0` pipeline with LLVM's `buildO0DefaultPipeline`, which includes the always-inliner, so inlining at
`-O0` is expected and is confirmed by a prototype before anything else is converted (the first implementation step).
If a prototype shows it does not hold with debug information on, the fallback is the compiler emitting the bounds
check and address arithmetic inline for a role array, which keeps 0025's addressor for everything else.

## Consequences

- **Compatibility.** No source, diagnostic or trap text changes: `array index out of bounds`, `array not empty`,
  the runtime's allocation overflow texts, allocation and free sizes, growth, destruction order, evaluation
  order, copy-on-lookup of nested values. A characterization suite pinning each is written and passes on the
  builtin array **before** the switch, then must pass unchanged after it.
- **Compile time.** The prelude is parsed, resolved and checked for every compilation. Measured before and after
  with the hello program; the bound is stated in the plan log.
- **Size.** Each concrete `Array<X>` instantiates a few small members. The bound is the 0025 gate: `hello` and
  the per-program executable do not grow beyond a stated limit, and the `--emit-llvm` comparison after type-id
  normalization is reported as a diagnostic.
- **Runtime.** The `joyeer_array_*_abi` symbols and their C declarations stay, because they are the public ABI and
  the library calls them. Nothing in the compiler emits calls to `append`, `remove_last` or `consume_empty` itself.
- **Wire and verifier.** Wire version 20 becomes 21 (the structure's element type, the new opcodes' removal, the
  new role). Malformed-IR tests cover the removed opcodes and the new field.
- **Dictionary.** Unchanged. `Dict` stays builtin until Phase 4; its values and keys may be `Array` types, which
  reach the clone and destroy helpers like any struct.

### Tests (written first)

- **Characterization, on the builtin array, before the switch** (the fixtures are then the acceptance test):
  every array diagnostic as a golden; trap texts and the index-out-of-bounds trap order against later
  arguments; destruction order of `[Resource]` with an element `deinit` that prints its index, at the end of a
  scope, on overwrite, on `?` and on an early return; `removeLast` of copyable, noncopyable and linear elements;
  `consumeEmpty` of an empty and a nonempty array; arrays of every zero-state element class of 0025 section 4;
  `[Void]`; nested arrays and dictionaries as elements; arrays of `JoinHandle` through a task; a constant array at
  every use (read, copy, spawn input and result); `main(args:)` and the native `[String]` and `[UInt8]`
  parameters and results; the printed type name in diagnostics.
- **Library:** shape and wire tests for the real declaration; the executed fixtures above at `-O0` and `-O2`
  compared exactly (stdout, exit status, the allocation counter at zero).
- **Cost gate** (both profiles, the paired bootstrap of `tests/perf/README.md`): the five `array_*` programs, and
  `hello` size; reported in the plan log.

## Open questions

1. Whether the prototype confirms always-inlining at `-O0` with line-table and full debug info (Section 3).
2. Whether instance discovery is narrowed to demanded instances (size), decided by the measurement.
3. Whether a later change should reimplement the algorithms generically (Option A2) for inlining at `-O2`; out
   of scope here, and only if the benchmarks justify it.
4. How `address_of` and the other byte-pointer intrinsics that name `[UInt8]` are described once the type kind is
   gone (by the `arrayElement` mark, as above); the table in `intrinsics.h` changes, the spelling does not.

## Review

Not yet reviewed. A language-designer review is required before acceptance, as for 0022 to 0025.

## Evidence

The inventory above (2026-11-15) and the baseline of `tests/perf/array_*.joyeer` in the plan 0011 log. Prior art:
0024's role binding of syntax, 0025's capabilities and its Option A3/B2/C2/D1 choices, 0023's bundled-prelude
separation.
