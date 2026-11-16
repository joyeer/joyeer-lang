# 0027: `Dict<K, V>` as a library struct

- Status: proposed (2026-11-16, revision 2 after one language-designer review of revision 1; not yet accepted)
- Spec: [§2.6](../../specs/02-types.md#26-generics) and §2.6.1 (lookup), §4.1.1 (copyability),
  [§20.5](../../specs/20-protocols.md) (key constraints), §20.12.5 (layout intrinsics and roles) and
  [§21.2.2](../../specs/21-concurrency.md) (task transfer) change with this record once it is accepted.
- Plan: [0011](../plan/0011-library-containers.md), Phase 4. Builds on [0025](0025-library-collection-capabilities.md)
  and [0026](0026-library-array.md) and **amends 0025's dictionary paragraph** (Decision 3); relates to
  [plan 0012](../plan/0012-equatable.md).

## Context

`Array` is now a library struct bound by a role (0026). `Dict<K, V>` / `[K: V]` is the last compiler-defined generic
container. Phase 4 asks for the same move: the buffer and operations in Joyeer, the dictionary-specific compiler
paths removed, no change a program can observe except where this record lists it, and no regression outside measured
noise. Inventory (2026-11-16, checked against the code by the review):

- **Runtime.** `lib/Runtime/core/dictionaries.joyeer` already implements the algorithms in Joyeer behind
  `joyeer_dictionary_{create_owned,clone,set_owned,find,at,destroy}_abi`. A dictionary is a 24-byte handle
  `{data, count, capacity}`; `data` points just past a 72-byte header `{keySize, valueSize, entrySize, valueOffset,
  keyKind, cloneKey, destroyKey, cloneValue, destroyValue}`, followed by `capacity` entries, key at offset 0, value
  at `valueOffset`. **Lookup is a linear scan** comparing keys by the runtime key kind (`Int`, `Bool`, `UInt8`,
  `String`; anything else by `memcmp`). Creation from a literal collapses duplicate keys, keeping the last value
  and destroying the earlier key and value. The entry layout is computed by the backend today
  (`native_collections.cpp`: `valueOffset = alignTo(keySize, valueAlign)`, `entrySize = alignTo(valueOffset +
  valueSize, max(keyAlign, valueAlign))`).
- **Compiler.** `TypeKind::dictionary` has 27 uses (12 in the type checker, 7 in `ir.cpp`, 3 in lowering, 5 in the
  backend) and these dictionary-specific sites:
  - *Members and operations:* the builtin members `count` and `get` (`nameresolution.cpp`), the checker's `get`
    special case with its `copy-of-noncopyable` text, `get`'s lowering, opcode `dictionaryGet`, verifier arm and
    backend arm; opcodes `constructDictionary`, `dictionarySet`, and the dictionary arm of `subscript_address`
    (`joyeer_dictionary_at_abi`, trap `dictionary key not found`).
  - *Binding and display:* `Dict` and `[K: V]` bound by spelling (`nameresolution.cpp`, `typechecking.cpp:64`,
    `129`), display `[K: V]` and syntactic `Dict<K, V>` (`typechecking.cpp:327`, `platform.cpp:68`).
  - *Policy:* `isLinear` and `noncopyableReason` over both arguments, `requireNoLinearDictionary`, the `Never`
    rejection (0026 A11), `rejectInvalidDictionaryKey`, the compound-assignment rejection, the literal checker.
  - *IR and backend:* copy-dependency edges and the requires-destroy visitor in `ir.cpp`, the element-address escape
    allow-list naming `dictionarySet` and `dictionaryGet`, copy and destroy by type kind in `native_ownership.cpp`,
    the layout in `native_types.cpp`, and wire type slot 11.
- **Observable behavior to keep** (§2.6.1, `supported-features.md`): `d[key]` read traps on a missing key;
  `&d[key] = value` inserts or replaces (the `&` is required for assignment through a subscript); `&d[key].f()`
  mutates a stored value; `d.count`; `get(key:)` returns an independent owned `Optional<V>`, is `.None` for a missing
  key, never flattens, and is rejected for a noncopyable value; compound assignment to an element is rejected; a
  dictionary cannot hold a linear or a `Never` key or value.
- **Checked: none today.** Constants (`constants.cpp` is array-only), native marshalling (no dictionary support;
  stays `unsupported-native-signature`), `for` over a dictionary (not implemented; `not-iterable`), key enumeration.
- **Key admission is late.** The checker rejects only function-type and type-parameter keys. `[Point: Int]` and
  `[Double: Int]` pass the checker and fail only in the backend (`Builder::keyKind`, `llvm.unsupported-type`, empty
  span) when a dictionary is constructed, read or assigned; a function that merely takes `[Point: Int]` compiles.

What differs from `Array` and so needs new compiler capability:

1. **The subscript key is a `K`** and is borrowed; the runtime takes a key *address*.
2. **`&d[key] = value` is an insert-or-replace**, not a store through an address.
3. **Key equality** comes from a key kind the compiler writes into the header.
4. **`get`** needs a clone of `V`, which an unconstrained `V` cannot call (no conditional methods, §20.10).
5. **A literal has key and value per entry**, with the duplicate-key rule and the entry layout above.
6. **Two owned type parameters**; the role machinery is built for one (see Decision 3).

## Options considered

### A. A library struct over the existing runtime ABI (the 0026 shape)

`dict_type` role on `public struct Dict<K, V>` in the existing `collections.joyeer`, three words of inert-zero
storage, method roles over the unchanged `joyeer_dictionary_*_abi`. The key kind is a per-instance constant. Accepted
key types, the linear scan, traps, texts, allocation sizes and growth do not change.

### B. A generic implementation over `K: Equatable` (and later `Hashable`)

Needs `Equatable` (plan 0012, all unchecked), changes the accepted key types, and mixes a hashing redesign into the
migration, which the plan defers to its own decision. It would replace `find_abi` (which takes a key kind) and the
header's `keyKind` with a compare callback, so the **roles** survive but **the ABI changes** in that later decision.

### C. Leave `Dict` builtin

No risk, but `get`, `count`, the display, the key kinds and the opcodes stay special and the plan's definition of
done is unmet.

## Decision (proposed)

Option **A**. Rules, subject to review:

1. **Binding.** `[K: V]`, dictionary literals and `Dict<K, V>` bind to the `dict_type` role symbol, never to a
   spelling; a user type named `Dict` shadows only the spelling and is qualified in diagnostics, as in 0026.
   `Dict` lives in the **existing `collections.joyeer`**: no fourth prelude, no new source ID, no packaging change.
2. **Unchanged behavior** (see the list above): the linear scan, traps and texts, allocation and growth, `get`,
   the noncopyable-value rejection, the compound-assignment, linear and `Never` rejections.
3. **Capabilities**, each tied to a role and never to a name, with closed shape validation:
   - `dict_type` structure role (`StructureRole::dict`; two owned type parameters; affine-only; read-only `count`).
   - `dict_element` addressor: `private borrowing func name(key: K): Int` returning the stored value's address,
     trapping `dictionary key not found` on a missing key. **One** addressor serves read, borrow, consume and mutate
     (and projection bases such as `&d[k].f()`), as 0025 specified.
   - `dict_set`: `private mutating func name(key: consuming K, value: consuming V): Void`, insert-or-replace. This
     **amends 0025**, which proposed an "inserting addressor for the final target of a plain `=`": a method taking the
     owned key and value matches `joyeer_dictionary_set_owned_abi`, needs no uninitialized slot and keeps "an RHS trap
     before the insert inserts nothing". `&d[k] += 1` still does not insert (it is rejected today, and stays so).
   - `dict_get`: `private borrowing func name(key: K): Optional<V>`, copyable `V` only, with the
     `copy-of-noncopyable` diagnostic kept; this is what lets `dictionaryGet` and its lowering go.
   - `dict_copy` (as `array_copy`) and `dict_literal` (a consuming method on an inert zero value, as 0026 A1).
     No `container_end`: a dictionary is affine-only.
   - **A read-only borrowed-address intrinsic**, `joyeer_intrinsic_borrow_address<T>(value: place): Int`, in
     authorized units only, accepted only as a direct argument of a call, valid for that call, with no escape, and
     accepting a `borrowing` parameter (which `place_address` rejects). It exists because the key is borrowed and the
     runtime wants its address; without it the only route is `stack_slot` + `clone_at`, which allocates a `String`
     key on every lookup and changes the allocation counter. Exclusivity: it is a read access of the place for the
     duration of the call. It must exist before the Stage 2 prototype so the prototype is like for like.
   - **`key_kind<K>()`**: an intrinsic folded to an integer constant during lowering for each concrete instance (1
     `Int`, 2 `Bool`, 3 `String`, 4 `UInt8`); no new opcode, verifier rule or wire slot. The runtime keeps comparing
     by key kind: a library cannot compare keys without an `Equatable`-like capability, so comparing in the library is
     not viable now.
4. **Key admission moves to the checker (a visible change, to be accepted explicitly).** With per-instance discovery
   a late failure would hit every mentioned `Dict<K, V>`. So the checker rejects a key type other than `Int`, `Bool`,
   `UInt8` and `String` at the type, with a span, as `type-checking.invalid-type-argument` and the help the
   type-parameter case already gives (spec 20.5). A program that merely mentions `[Point: Int]` and previously
   compiled now fails; one that used it failed in the backend before. The characterization PR pins today's
   behavior first.
5. **Generalizing the role table.** The array role is hard-wired to one parameter and an `Int` index
   (`isArrayRoleType`, the 4-entry method table, the `index` label, the verifier's single array symbol, the
   addressor shape, the copier and `copyDependents` using `arguments[0]`, the literal verifier). The infrastructure
   stage replaces these by a role record: owned type parameters, the addressor's key type, the method-role set. It
   is a refactor of the array paths with their existing tests green first, then the dictionary role added to it.
   New wire version 24: `StructureRole::dict`, the new method roles and function flags; reserved slots after the
   switch are type slot 11 and the opcodes of `constructDictionary`, `dictionarySet`, `dictionaryGet`.
6. **Seam.** A replaced collections prelude need not declare a dictionary: the `dict_type` role is required only when
   a program mentions a dictionary (`invalid-prelude` at first use), so the 59 role tests and the probe prelude keep
   working. The Stage 2 prototype replaces the whole collections prelude, so it also contains a working array.

### Lowering of `&d[k] = v` (observable ordering)

Today the base address is taken first, then key, then value, then the store (`lowerDictionarySet`). Arrays defer the
element address until after the right-hand side because it may reallocate. For `dict_set`: evaluate the base place,
the key and the value to owned temporaries first, left to right (an error or trap before the insert inserts
nothing); then take the receiver address and call `dict_set`. For a nested `&outer[a][b] = v` the outer element
address is also deferred until after the value, so `grow(&outer)` on the right cannot leave it dangling. The
characterization PR pins `&d[d.count] = 1`, `&d[k] = f(&d)` and the nested form on the builtin; if any is accepted
today and differs afterwards, that is listed as a change.

### Residual compiler special cases after the switch (honestly)

1. The dict role itself: shape validation, two owned parameters, affine-only policy, read-only `count`.
2. Key admission: the `key_kind` table, function-type, type-parameter and non-admitted key rejection.
3. Role-keyed `linear-collection-element`, `Never` rejection, compound-assignment rejection and the `get`
   noncopyable diagnostic. (`get`'s member, checker site, opcode and lowering go with `dict_get`.)
4. The display `[K: V]`, the syntactic `Dict<K, V>` display, and binding of `[K: V]` and literals to the role symbol.
5. Literal lowering and packing, the owned key and value argument classification, and the `alwaysInline`
   closed-wrapper policy extended by role (0026 A4).
6. Sendability by role (a dictionary is a task input exactly when `K` and `V` are; §21.2.2), recursive-layout
   indirection for the role, and the escape-rule allow-list.

### Process (the lessons of 0026)

- **Measure before gates.** No dictionary benchmarks exist. `tests/perf` gains `dict_*` programs and flat probe pairs
  like `array_index_flat`: lookup at sizes 1, 4, 16 and 64 with `Int` and `String` keys, hit and miss; insert with
  growth across 4 and 8; replacement; a literal with duplicates; nested destruction. A builtin-against-builtin run
  records the noise first. The gate **structure** is predeclared: at `-O2` no regression outside noise (lower bound
  of the 95% interval at most 1) and a stated median bound; at `-O0` a median bound, and any `-O0` amendment of
  0025's gate requires the user's explicit acceptance naming the number first proposed; executable size per program
  and for `hello`; `hello` front-end time (the collections prelude grows); a rerun rule and a missed-gate rule as in
  0026 Section 4. The numbers are fixed after the prototype is measured, with the first number proposed recorded.
- **Cost mechanism.** The existing always-inline policy extended to the dictionary wrappers by role, not by a
  dictionary-specific mark; the addressor adds a key spill and parameter allocas at `-O0`, so the ratio is measured,
  not assumed.
- **Debug scopes.** 0026's known gap (a) is open. Before the switch PR: either close it or add a dictionary e2e at
  `-O1` and `-O2` with `-gfull` and a native execution check.
- **Differential testing** against the builtin baseline compiler at `-O0` to `-O3`, with allocation-counter equality
  (no key clone on lookup).

### Staging (each revertable)

1. **Characterization PR** (tests only, passing on the builtin): every dictionary diagnostic as a golden; the trap
   text and order against later arguments; `d.count`; `&d[f()] = g()` with side effects and an RHS trap that inserts
   nothing; the three reallocation-sensitive forms above; literal duplicates for `Int` and `String` keys with
   resource values (destruction order of the dropped key and old value); `get` for nested optionals, `Void` values and
   a noncopyable `V`; `&d[k].append(element: x)`; `[String: [Int]]` copy independence; `Dict` in `Array` and the
   reverse; recursive types through a dictionary; a user `Dict`; `extension Dict`; `[Point: Int]`, `[Double: Int]` and
   a function that takes `[Point: Int]`; `unit_dictionary_missing`; a dictionary as task input and result; `for` over
   a dictionary; a native dictionary parameter; `==`; the linear and `Never` rejections; destruction order of a
   `[K: Resource]`.
2. **Infrastructure PR, part 1:** generalize the role table with the array tests green; the borrowed-address
   intrinsic and `key_kind`, with their tests (accepted only as a direct call argument, `-O2` ordering, rejected
   outside authorized units, per-type constants).
3. **Measurement PR:** the prototype over the new capabilities, the benchmarks, the gate numbers, the noise run.
4. **Infrastructure PR, part 2:** the dictionary roles, shape validation, wire 24, the seam; `Dict` untouched.
5. **Switch PR:** bind `[K: V]`, the real `Dict`, literals, `get`, wire reservations, sendability; differential runs.
6. **Cleanup PR:** delete `TypeKind::dictionary`, the three opcodes and the backend cases.

## Consequences

- **Visible changes** (to be listed, pinned and regenerated with the project's tools, as in 0026): `d.address` and
  `d.capacity` (and the compiler-called methods) become `inaccessible-declaration`; `==` on dictionaries is rejected
  by the checker (today it type-checks and fails in LLVM); a user type named `Dict` is qualified in diagnostics;
  non-admitted key types are rejected at the type (Decision 4); `--report-instantiations` may list dictionary
  instances.
- **Spec.** §2.6 (drop "only `Dict` remains compiler-defined", add the role paragraph), §2.6.1 (the library `get`),
  §4.1.1, §20.5, §20.12.5 (roles, the capability table row, the two intrinsics), §21.2.2 and line 112 (a dictionary
  as a task input), and the §20 text on `Dict.get` witnessing a requirement. No grammar change.
- **Docs.** `supported-features.md` (containers, key rows, lookup), `docs/impl/{ir, backend, runtime,
  name-resolution, type-checking}.md`, and the plan's wording ("implement their existing operations in Joyeer" is
  already true of `dictionaries.joyeer`; the deliverable is the library type and the removal of compiler paths).
- **Amendment of 0025.** `dict_set` replaces the "inserting addressor"; 0025's Phase 4 questions on the insert path
  are closed by it.

## Open questions

Revision 1's three questions were answered by the user on 2026-11-16 (the recommendations of revision 2), and are
now rules above:

1. The borrowed-address intrinsic is acceptable as specified: read-only, call-scoped, a direct call argument only,
   `borrowing` places allowed (Decision 3).
2. Non-admitted key types are rejected at the type in the checker, accepting that a program which only mentions
   `[Point: Int]` stops compiling (Decision 4).
3. The role table is generalized into one role record; no parallel dictionary path (Decision 5).

None remains open. Revision 2's review may raise more.

## Review

Revision 1 was reviewed 2026-11-16 by the language designer: revise. Folded into revision 2: `count`, `get` and the key
address capability added; the false key-admission claim corrected; the assignment form fixed to `&d[key] = value`;
the contradiction with 0025 made an explicit amendment; the role table generalization, the seam, `dict_set` ordering,
the wire version, the residual list, the process lessons and the visible changes stated; "constants if any" and
`container_end` removed. Revision 2 has not been reviewed; the user answered its three open questions on 2026-11-16 (rules above).

## Evidence

The inventory above; prior art 0024, 0025 (the access-mode lowering of the dictionary subscript), 0026 (the array
switch, its measurement method and its lessons).
