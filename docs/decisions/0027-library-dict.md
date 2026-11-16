# 0027: `Dict<K, V>` as a library struct

- Status: proposed (2026-11-16, revision 1; not yet reviewed)
- Spec: [§2.6](../../specs/02-types.md#26-generics) and §2.6.1 (lookup), [§4.3](../../specs/04-memory.md),
  [§20.12.5](../../specs/20-protocols.md) change with this record once it is accepted.
- Plan: [0011](../plan/0011-library-containers.md), Phase 4. Builds on [0025](0025-library-collection-capabilities.md)
  and [0026](0026-library-array.md); relates to [plan 0012](../plan/0012-equatable.md).

## Context

`Array` is now a library struct bound by a role (0026). `Dict<K, V>` / `[K: V]` is the last compiler-defined
generic container. Phase 4 asks for the same move: the dictionary's buffer and operations in Joyeer, the
dictionary-specific compiler paths removed, no change a program can observe, and no regression outside measured
noise. An inventory of the code (2026-11-16):

- **Runtime.** `lib/Runtime/core/dictionaries.joyeer` (312 lines) already implements the algorithms in Joyeer behind
  the public ABI `joyeer_dictionary_{create_owned,clone,set_owned,find,at,destroy}_abi`. A dictionary is a 24-byte
  handle `{data, count, capacity}`; `data` points just past a 72-byte header `{keySize, valueSize, entrySize,
  valueOffset, keyKind, cloneKey, destroyKey, cloneValue, destroyValue}`, followed by `capacity` entries, a key at
  offset 0 and a value at `valueOffset`. **Lookup is a linear scan** that compares keys by a runtime *key kind*
  (`Int`, `Bool`, `UInt8`, `String`; other kinds by `memcmp`). Creation from a literal collapses duplicate keys,
  keeping the last value.
- **Compiler.** `TypeKind::dictionary` has about 27 uses: 12 in the type checker (registration `Dict`, key
  validation, `get(key:)`, the subscript, literals, compound-assignment rejection, the linear and `Never`
  rejections), 7 in `ir.cpp`, 3 in lowering, 5 in the backend. The IR opcodes are `constructDictionary`,
  `dictionarySet`, `dictionaryGet` and the dictionary case of `subscript_address`, which calls
  `joyeer_dictionary_at_abi` and so traps `dictionary key not found`. Copy and destroy go through
  `joyeer_dictionary_{clone,destroy}_abi` by type kind (`native_ownership.cpp`).
- **Observable behavior to keep** (spec §2.6.1, `supported-features.md`): `d[key]` read traps on a missing key;
  `d[key] = value` inserts or replaces; `&d[key].append(element: x)` mutates a stored value; `get(key:)` returns an
  independent owned `Optional<V>` that is `.None` for a missing key, never flattens nested optionals, and is
  rejected for a noncopyable value; compound assignment to an element is rejected; a dictionary cannot hold a linear
  key or value, or a `Never` one (0026, A11); keys are limited to `Int`, `Bool`, `UInt8` and `String`.

What differs from `Array` and so needs new compiler capability:

1. **The subscript key is a `K`, not an `Int`.** The `array_element` role takes an `Int` index and returns a
   borrowed element address. A dictionary needs the same with a key parameter of type `K`.
2. **`d[key] = value` is not an element place.** The key may not exist, so it is an insert-or-replace, a call
   rather than a store through an address.
3. **Key equality.** The runtime picks a comparison from the key kind, a number the compiler writes into the
   header. A library struct needs the key kind of `K` as a compile-time value, or an equality it can call.
4. **A literal with keys and values**, evaluated and moved like an array literal (0026), with the duplicate-key rule.

## Options considered

### A. A library struct over the existing runtime ABI (the 0026 shape)

`dict_type` role on `public struct Dict<K, V>` in `collections.joyeer`, three words of inert-zero storage, method
roles analogous to 0025/0026 (`dict_element` with a key parameter, `dict_set`, `dict_copy`, `dict_literal`,
`container_end` if needed), calling the unchanged `joyeer_dictionary_*_abi` functions. The key kind comes from a new
layout intrinsic `key_kind<K>()` that the compiler evaluates per instance (`Int`, `Bool`, `UInt8`, `String`;
anything else is rejected where the key type is checked today). Accepted key types, the linear scan, traps, texts,
allocation sizes and growth do not change. Cost: one new intrinsic, three or four new roles and their shape
validation, a key parameter on the addressor, an insert call for assignment. Failure mode: the inliner must keep the
`d[key]` path as cheap as today at `-O0` (0026 needed the always-inliner for the same reason).

### B. A generic Joyeer implementation over `K: Equatable` (and later `Hashable`)

The algorithms written generically, with equality supplied by [plan 0012](../plan/0012-equatable.md) and an optional
hash table instead of a scan. This is the end state the plan hints at, but it needs `Equatable` first, changes which
key types are accepted, and mixes a performance redesign (hashing) with the migration. The plan lists a hash redesign
as a separate decision ("record a decision for key constraints and hashing/equality before broadening accepted key
types").

### C. Leave `Dict` builtin

No risk and no work, but the plan's goal (no container-specific type kinds, opcodes or backend cases) stays unmet and
the key-kind plumbing stays special.

## Decision (proposed)

Option **A**, shaped so that B can replace `key_kind<K>()` with an `Equatable` constraint later without changing the
roles or the runtime ABI. Specifically, subject to review:

1. `[K: V]` and `Dict<K, V>` bind to the `dict_type` role symbol, never to a spelling, as `[T]` binds to
   `array_type` (0026 Section 2). A user type named `Dict` shadows only the spelling.
2. The accepted key set, the linear scan, the traps (`dictionary key not found`), `get(key:)`, the
   noncopyable-value rejection, the compound-assignment rejection and the linear and `Never` rejections are
   **unchanged**. Broadening keys, hashing and borrowing lookup of noncopyable values are separate decisions.
3. New compiler capability, each tied to the role and never to a name, with closed shape validation like 0025's:
   the `dict_element` addressor (key parameter), a `dict_set` method for `d[key] = value`, a `dict_literal` method,
   the `key_kind<K>()` intrinsic (runtime units only, evaluated per instance), and the structure role `dict`.
4. The runtime ABI and the header layout stay; the library calls them. The `Dict` handle stays three `Int` words.
5. Gates are **not proposed here**. 0026 lesson: numbers proposed before a measurement were later loosened. A
   prototype (a minimal library dictionary over the ABI, like `array_buffer_prelude.joyeer`) is measured against the
   builtin first: construction, lookup (hit and miss), insertion and replacement, growth, nested destruction, in both
   profiles, with a rerun rule and a missed-gate rule as in 0026 Section 4, and the numbers are fixed in the next
   revision.

### Staging (each revertable)

1. **Characterization PR**: tests on the builtin dictionary, passing today (every diagnostic as a golden, traps and
   their order, destruction order of dictionaries of resources, duplicate-key literals, `get` for nested optionals,
   `&d[key].mutate()`, `Dict` inside `Array` and the reverse, shadowed `Dict`, recursive types through a dictionary).
2. **Measurement PR**: the prototype, the benchmarks, the gate numbers.
3. **Infrastructure PR**: roles, `key_kind`, the structure role and wire change, packaging; `Dict` untouched.
4. **Switch PR**: bind `[K: V]`, the real `Dict`, literals, constants if any, wire bump.
5. **Cleanup PR**: delete `TypeKind::dictionary`, the three opcodes and the backend cases.

## Consequences

- **Compatibility.** No source change; the few diagnostics that move (private members become
  `inaccessible-declaration`, as in 0026) are listed and pinned.
- **Testing.** Differential testing against the builtin baseline compiler, as for `Array`, before the switch merges.
- **Documentation.** Spec §2.6, §2.6.1, §20.12.5; `docs/impl/{supported-features, ir, backend, runtime,
  name-resolution, type-checking}.md`; the plan log.

## Open questions

1. Does the `dict_element` addressor return the stored value's address for a *read* and for a *mutating projection*
   (`&d[key].append(element: x)`) by the same role, as `array_element` does, or does a mutating projection of a
   missing key need its own rule? (Today a missing key traps in both.)
2. Is `key_kind<K>()` the right seam, or should the library compare keys itself for the four accepted kinds and keep
   the runtime's `keyKind` only for clone and destroy? The second removes an intrinsic but costs inlined comparisons.
3. Should `d[key] = value` stay a distinct `dict_set` role, or be lowered through `dict_element` when the key exists
   and `dict_set` otherwise? (The first is simpler and matches the runtime's `set_owned`.)

## Review

Not yet reviewed. Required before acceptance: language designer; type-system and IR/backend reviewers on the
infrastructure and switch PRs.

## Evidence

The inventory above (2026-11-16); prior art 0024 (role binding of syntax), 0025 (capabilities), 0026 (the array switch,
its measurement method and its lessons).
