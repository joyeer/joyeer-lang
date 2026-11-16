# 0027: `Dict<K, V>` as a library struct

- Status: accepted for stages 1-4 (2026-11-16, revision 4, by the user, after three language-designer reviews, the
  last "accept with changes"). Stages 5-7 are conditional on the measured gates (Acceptance condition below); the
  three open questions of revision 2 were answered by the user the same day.
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
   - `dict_type` structure role (`StructureRole::dict`; two owned type parameters; affine-only; read-only `count`;
     `[:]` allocates a header today, so the inert-zero literal receiver is a moved-from value only, as for `[]`).
   - `dict_element` addressor: `private borrowing func name(key: K): Int` returning the stored value's address,
     trapping `dictionary key not found` on a missing key. **One** addressor serves read, borrow, consume and mutate
     (and projection bases such as `&d[k].f()` and `&d[k][i] = v`: a missing key traps and never inserts), as 0025
     specified. The synthesized call passes the key operand raw; an owned key temporary (`d[makeKey()]`) is destroyed
     exactly once and not before the returned address is used (a test).
   - `dict_set`: `private mutating func name(key: consuming K, value: consuming V): Void`, insert-or-replace; the library
     `move_in`s the owned key and value into `stack_slot`s (as `append` does) and passes their addresses, never
     `borrow_address`, because the runtime adopts the bytes. This
     **amends 0025**, which proposed an "inserting addressor for the final target of a plain `=`": a method taking the
     owned key and value matches `joyeer_dictionary_set_owned_abi`, needs no uninitialized slot and keeps "an RHS trap
     before the insert inserts nothing". `&d[k] += 1` still does not insert (it is rejected today, and stays so).
   - `dict_get`: **`public borrowing func get(key: K): Optional<V>`**, the one public dictionary method role, so
     `d.get(key: k)` is an ordinary call and no compiler rewrite by name is needed. The role adds only the call-site
     `copy-of-noncopyable` check, keyed on the resolved callee's role (today it is keyed on `name == "get"` at
     `typechecking.cpp:4946`, `irlowering.cpp:3175`), reported before any instance is demanded, so a noncopyable `V`
     never reaches `clone_at` (the verifier rejects it). `dict_get` is demanded only by user calls, not by instance
     discovery. This lets `dictionaryGet` and its lowering go. All other dictionary method roles are private.
   - `dict_copy` has the shape of `array_copy` and `dict_literal` the shape of `array_literal` (a consuming method on
     an inert zero value, 0026 A1): every role is per owner, one naming scheme, so an author never writes
     `@compiler_role(array_copy)` on a dictionary. The checks of `array_copy` and `container_end` that hard-code
     `owner->libraryRole == arrayType` and one owner parameter are generalized in stage 2a. No `container_end` for
     the dictionary itself: it is affine-only.
   - **The literal's storage (the dictionary literal design).** The runtime wants interleaved entries (key at offset
     0, value at `valueOffset = alignTo(keySize, valueAlign)`, `entrySize` rounded up to the larger alignment). The
     prelude declares a private `DictEntry<K, V>` struct (a role, `dict_entry`, closed shape: exactly the fields `key:
     K` then `value: V`, no `deinit`), whose ordinary struct layout *is* that layout. The literal packs into
     `stack_block<DictEntry<K, V>>(count: N)`. The closed packing protocol of 0026 A3 (`ir.cpp:1479-1608`) is extended
     by these rules, assigned to stage 5, with malformed-IR tests asserting which rule fired:
     1. The factory is a `dict_literal` role method; its receiver has role `dict` and its result two arguments (K, V).
     2. The block's element type E is an IR structure with role `dictEntry`: exactly two fields, of types K then V.
     3. Each slot is `slot_address(block, i)` then one `integer_address` of type E, as today. The place's only uses are
        `field_addr` operations of distinct field symbols (0 and 1), in the same block, between the storage and the
        transfer; `field_addr` joins the allowed opcodes of the packing window.
     4. Each `field_addr` result has exactly one use, a `store` of exactly the field's type, evaluated before the
        storage (the existing `evaluatedFirst` rule).
     5. Coverage: the key field is stored once for every `0 <= i < N`; the value field is stored once for every `i`
        unless V is `Void`, for which no value-field `field_addr` or store is allowed. `N == 0` means no slots.
     6. No loads, no escaping address, no fallible operation inside the window, one factory call after packing.
     The `literalFactories` shape (`ir.cpp:810-840`), the element rule (`ir.cpp:1516-1517`) and the per-place rule
     (`ir.cpp:1550-1593`) are generalized from "array, one argument" to this. The wire serializes `StructureRole::dict`,
     `dictEntry` and the literal-factory flag. The library passes `size_of<DictEntry<K, V>>()`
     as `entrySize`, `size_of<K>()` and `size_of<V>()`, and computes `valueOffset` with the same formula; a test pins
     that formula against the compiler's layout of `DictEntry` for every admitted key and a spread of values (`Void`,
     `UInt8`, `Int`, `String`, structs, enums). The backend no longer computes the layout. The checker **interns `DictEntry<K, V>` for every concrete `Dict<K, V>`** (`requireDeinitInstances` skips structs
     without a `deinit`, so it would not otherwise); the entry type and its two field symbols are stored in the role
     record and lowering takes them from it. The role validator needs a per-role visibility rule: `dict_entry` is
     `private`, `array_type` is `public`. The entry is never a value: only addresses, `field_addr` and raw stores reach
     it, so nothing clones or destroys it implicitly (but `emitOwnership` creates internal helpers for it that count
     in the size gate). A backend invariant at type definition asserts, for every `dictEntry` structure, that LLVM's
     offset of field 1 equals `alignTo(layout(K).size, layout(V).alignment)` and its alloc size equals
     `layout().size`, so the library formula and the placement agree by construction. Rejected: changing
     `joyeer_dictionary_create_owned_abi` to take key and value runs, or two parallel blocks (a public ABI change, or a
     heap-allocated entry buffer with a different allocation counter), and lowering the literal to N `dict_set` calls
     (changes capacity, growth, duplicate-key destroy order and the allocation counter).
   - **A read-only borrowed-address intrinsic**, `joyeer_intrinsic_borrow_address<T>(value: place): Int`, in
     in the bundled preludes and `--runtime-module` units only (rejected as `unauthorized-intrinsic` in user files, like
     the other layout intrinsics, §20.12.5), accepted only as a direct argument of a call. It yields the address of the local storage
     holding the value of a named readable place (a local, any parameter, or a field path of those). For a
     `borrowing` parameter that storage is a callee-local copy aliasing the caller's (as the backend's `spill(key)`
     does today: not an allocation); for a `String` key it is the handle's address, so `keySize` is `size_of<String>`.
     It is valid until the enclosing call returns. Rvalues and constants are rejected; user-level rvalue keys never
     reach it, because the library applies it to the addressor's own `key` parameter. It is read-only by the library
     author's duty, enforced by the checker's direct-argument rule exactly as for `place_address`; hand-written IR is
     not verified. Exclusivity: a read access of the place for the call, added to `collectCallArgumentAccess` beside
     `place_address` (and to `permittedPlaceAddress` and the three copies of the intrinsic-name list); `f(a:
     borrow_address(value: self.k), b: place_address(value: &self))` is `overlapping-access`. The user-level forms
     `&d[k] = f(&d)`, `d.get(key: d.keyField)` and `&d[d[1].id].bump()` follow ordinary borrowing rules and do not
     involve the intrinsic. Without it the only route is `stack_slot` plus `clone_at`, which allocates a `String`
     key on every lookup and changes the allocation counter.
   - **`key_kind<K>()`**: folded to an integer constant during lowering for each concrete instance (1 `Int`, 2
     `Bool`, 3 `String`, 4 `UInt8`); no new opcode, verifier rule or wire slot. For any other `K` it is a **lowering
     error, never 0** (a zero kind would select the runtime's `memcmp` fallback, wrong for floats and padded
     structs). The runtime keeps comparing by key kind: a library cannot compare keys without an `Equatable`-like
     capability.
4. **Key admission moves to the checker (a visible change, accepted by the user 2026-11-16).** The admitted key
   types are exactly `Int`, `Bool`, `UInt8` and `String`, by type kind, not spelling (`Int32`, `Optional`, structs and
   `Double` are rejected). They are stated as a normative sentence in §2.6, labelled "closed until plan 0012". The
   checker rejects any other key at the type, with a span, as `type-checking.invalid-type-argument` and the help the
   type-parameter case already gives (§20.5), at all three sites, re-keyed to the role: an annotation `[K: V]`
   (the key's span), a nominal `Dict<K, V>` (the first argument's span) and a literal (the first entry's key, or the
   whole literal when empty). The rule skips function-type, type-parameter and `Never` keys, which already have their
   own diagnostics, so each mistake reports once. Both this kind rule and the existing type-parameter-key rejection do not apply inside the `dict_type` declaration
   (prelude source only), whose bodies write `Dict<K, V>`; user generics over `[K: V]` stay rejected (§20.5). A program that merely mentions
   `[Point: Int]` and previously compiled now fails; one that used it failed in the backend before. The
   characterization PR pins today's behavior first.
5. **Generalizing the role table (accepted by the user 2026-11-16).** The array role is hard-wired to one parameter
   and an `Int` index (`isArrayRoleType`, about 124 uses in 15 files; the 4-entry method table and the `index` label,
   `isOwnedArgumentContext`, the verifier's single array symbol and `addressedElement = arguments[0]`, the addressor
   shape, the copier and `copyDependents`, `DestructionAnalysis`, the literal verifier). The refactor replaces these
   by a role record (owned type parameters, the addressor's key type, the method-role set), with the array tests green
   first. It keeps `for-in`, native marshalling, constants, `String.utf8` and `consume-of-linear-element` **array-only**:
   a generalized predicate must not make `for x in dict` subscript with an index.
6. **Wire.** Wire 24 is bumped by the first PR that serializes the new roles, stage 5 (`StructureRole::dict` and
   `dictEntry`, the method roles, the literal-factory flag). The removed slots (opcodes 39-41, type slot 11) are
   rejected at the cleanup stage with wire 25, as 0026 held them.
7. **Seam.** The `dict_type` role is required only when a program mentions a dictionary (`invalid-prelude` at first
   use). The role tests that mention one (`collection_roles_test.cpp`, the `Buffer<[String: Int]>` test) either
   declare a `Dict` in their replaced prelude or are rewritten in the same stage; the other replaced-prelude users
   (`array-addressor-probe`, `tests/perf/*_library`) are checked. The prototype replaces the whole collections prelude,
   so it also contains a working array.

### Lowering of `&d[k] = v` (observable ordering)

§4.4.3 already says index operands, then the right-hand side, then the access. Today's `lowerDictionarySet` takes the
base address eagerly, before the key and value. For `dict_set`: evaluate the key operands left to right, then the
value, to owned temporaries; then take the base address chain, outermost first; then call `dict_set`. So `&d[k] =
d[k2]` and `&outer[a][b] = grow(&outer)` cannot leave an address dangling after a reallocation. **Visible change:**
`&outer[missing][b] = f()` now runs `f()` before the `dictionary key not found` trap. The characterization PR pins
`&d[d.count] = 1`, `&d[k] = f(&d)`, `&d[k] = consume d[k2]`, `&d[makeKey()] = v` where the call traps or returns, and
the nested form; the allow-list entries naming `dictionarySet` and `dictionaryGet` in the escape rule become plain
calls and are removed only at cleanup.

### Residual compiler special cases after the switch (honestly)

1. The dict role itself: shape validation, two owned parameters, affine-only policy, read-only `count`, `DictEntry`.
2. Key admission: the `key_kind` table, function-type, type-parameter and non-admitted key rejection.
3. Role-keyed `linear-collection-element`, `Never` rejection, compound-assignment rejection (`typechecking.cpp:5805`,
   re-keyed to the role) and the `get` checker special case (re-keyed from `name == "get"` plus the kind to the role).
4. The display `[K: V]`, the syntactic `Dict<K, V>` display, and the binding of `[K: V]` and literals to the role
   symbol (`nameresolution.cpp` three sites).
5. Dictionary literal lowering and the extended closed packing protocol, and the `alwaysInline` closed-wrapper policy
   extended by role (0026 A4).
6. Sendability needs no compiler change: a dictionary is a task input exactly when `K` and `V` are, by relocatability
   and the linear ban (§21.2.2); pinned by a `dict_task_transfer` fixture. Recursive-layout indirection for the role
   and the escape-rule allow-list remain.

### Process (the lessons of 0026)

- **Gates are stated now, as first proposed**, so they cannot be loosened after seeing the library result (0026's
  revision-2 failure). They apply to every `dict_*` program and every flat pair, in every debug mode, on Windows x64;
  Linux is measured and reported, and a jump of one segment page on Mach-O is reported, not gated (0026 A9). At `-O2`: median ratio at most 1.03 and the lower bound of the 95% interval at most 1. At
  `-O0`: median at most **1.10**; 1.15 is a ceiling reachable only by the user's explicit acceptance naming 1.10 as
  the first proposal. **This record amends 0025's `-O0` cost gate ("no regression outside noise") for dictionaries
  only, on the ground of 0026 Section 4 and the preamble (§0.1.2).** The array addressor reached 1.11 to 1.12 at `-O0`
  after every optimization, and the dictionary wrapper does more, so a miss at 1.10 is likely and a user decision
  will probably be needed; that is stated now. Size: at most +4 KiB at `-O0` and +2 KiB at `-O2` per `dict_*` program, `hello` no growth at
  `-O2`. `hello` front-end time at most +15% (the collections prelude grows). A rerun rule and a missed-gate rule as
  in 0026 Section 4. The `-O0` risk is real on tiny dictionaries (sizes 1 and 4): the wrapper adds a key alloca, the
  24-byte receiver by value and the `Optional` construction in `dict_get`.
- **Benchmarks first.** No dictionary baseline exists. `tests/perf` gains `dict_*` programs and flat probe pairs like
  `array_index_flat`: lookup at sizes 1, 4, 16 and 64 with `Int` and `String` keys, hit and miss; insert with growth
  across 4 and 8; replacement; a literal with duplicates; nested destruction. A builtin-against-builtin run records the
  noise. The prototype measures lookup, `get`, growth and drop through explicit method calls; `dict_set` and the
  literal arrive in the switch stage, which reruns every row (0026's lesson: a prototype that is not like for like
  misleads). Its lookup row is not like for like with `d[k]` (no role, no raw load in user code), so **the measurement
  PR gives a go or no-go verdict only and the gates bind at the switch**; it requires a margin at `-O0` (1.05) so the
  like-for-like rerun has headroom.
- **Cost mechanism.** The existing always-inline policy extended to the dictionary wrappers by role; measured, not
  assumed.
- **Instance discovery.** `requireDeinitInstances` demands deinit, the addressor, the copy method (if copyable), the
  literal method and `dict_set` for every concrete mentioned `Dict<K, V>`; `dict_get` is demanded only by user calls. If the size gate fails, narrowing discovery to
  demanded instances is the trigger carried over from 0026.
- **Debug scopes.** Closing 0026's known gap (a) is a **prerequisite of the switch PR**, both parts: a dictionary e2e
  at `-O1` and `-O2` with `-gfull` and a native execution check, and debug-info verification after the optimization
  pipeline in a test build.
- **Differential testing** against the builtin baseline compiler at `-O0` to `-O3`, with allocation-counter equality
  (no key clone on lookup).

### Staging (each revertable)

1. **Characterization PR** (tests only, passing on the builtin): every dictionary diagnostic as a golden; the trap
   text and order against later arguments; `d.count`; `&d[f()] = g()` with side effects and an RHS trap that inserts
   nothing; the reallocation-sensitive forms above; `consume d[k]`, `d[k]` as a borrowed argument, `let x = d[k]` of a
   noncopyable `V`, `&d[k].f()` and `&d[k][i] = v` (a missing key traps, never inserts); literal duplicates for `Int`
   and `String` keys with resource values (destruction order of the dropped key and old value); `get` for nested
   optionals, `Void` values and a noncopyable `V`; `&d[k].append(element: x)`, `&d[d[1].id].bump()`,
   `d.get(key: d.keyField)`; `[String: [Int]]` copy independence; `Dict` in `Array` and the reverse; recursive types
   through a dictionary; a user `Dict`; `extension Dict`; `[Point: Int]`, `[Double: Int]` and a function that takes
   `[Point: Int]`; `unit_dictionary_missing`, a non-empty `[K: Void]` literal with a duplicate key and an insert; `[String: Any]` and
   `[Any: Int]`; a dictionary as task input and result; `for` over a dictionary; a native
   dictionary parameter; `==`; the linear and `Never` rejections; destruction order of a `[K: Resource]`; the
   zero-handle destroy.
2. **Infrastructure PR, part 2a:** the role-table refactor only, no behavior change, the array differential and
   characterization suites green.
3. **Infrastructure PR, part 2b:** `borrow_address` and `key_kind` with their tests (direct call argument only,
   `-O2` ordering, `unauthorized-intrinsic` in user files, rvalues rejected, `overlapping-access`, per-type
   constants, the non-admitted lowering error), and the entry layout pin with an ordinary struct in a test prelude.
4. **Measurement PR:** the prototype, the benchmarks and the noise run, against the gates above.
5. **Infrastructure PR, part 3:** the dictionary roles and `dict_entry`, their shape validation (duplicate or missing
   roles, `dict_entry` with a `deinit`, a third field, wrong order or `public`, a `private` `dict_get`, `dict_set`
   labels `key`/`value`, extension, a nested `Dict<K, V>` in the prelude), the closed literal-protocol extension with
   its malformed-IR tests (missing or duplicate key or value store, wrong type, swapped fields, a load, an escaping
   field address, a non-entry element, wrong count, a wrong `Void` rule, a fallible operation in the window, a
   non-inert receiver, two transfers, a cross-block store, a hand-written `field_addr` outside the window), the
   backend layout invariant, wire 24, the seam; `Dict` untouched.
6. **Switch PR:** bind `[K: V]`, the real `Dict`, the literal, `get`, key admission, wire 24, sendability fixture;
   differential runs; the debug-scope prerequisite.
7. **Cleanup PR:** delete `TypeKind::dictionary`, the three opcodes, the backend cases and the allow-list entries;
   reject slots 39-41 and type slot 11 at wire 25.

## Consequences

- **Visible changes** (`&outer[missing][b] = f()` runs `f()` before the missing-key trap; the rest to be listed, pinned and regenerated with the project's tools, as in 0026): `d.address` and
  `d.capacity` (and the compiler-called methods) become `inaccessible-declaration`; `==` on dictionaries is rejected
  by the checker (today it type-checks and fails in LLVM); a user type named `Dict` is qualified in diagnostics;
  non-admitted key types are rejected at the type (Decision 4); `--report-instantiations` may list dictionary
  instances.
- **Spec.** §2.6 (drop "only `Dict` remains compiler-defined", add the role paragraph and the admitted-key sentence), §2.6.1 (the library `get`),
  §4.1.1, §20.5, §20.12.5 (roles, the capability table row, the two intrinsics), §21.2.2 (`21-concurrency.md` lines 78-80: a dictionary as a task input; the linear-ban bullet at line
  112 stays true), and the §20 text on `Dict.get` witnessing a requirement. No grammar change.
- **Docs.** `supported-features.md` (containers, key rows, lookup), `docs/impl/{ir, backend, runtime,
  name-resolution, type-checking}.md`, and the plan's wording ("implement their existing operations in Joyeer" is
  already true of `dictionaries.joyeer`; the deliverable is the library type and the removal of compiler paths).
- **Amendment of 0025.** `dict_set` replaces the "inserting addressor"; 0025's Phase 4 questions on the insert path
  are closed by it.

## Acceptance condition

As 0026 did: this record is accepted for stages 1-4 (characterization, the role-table refactor, the intrinsics and the
measurement). Stages 5-7 are conditional on the measurement meeting the gates above or on the user's explicit decision
about a missed gate; a missed gate is not renegotiated in a pull request.

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
`container_end` removed. Revision 2 was reviewed 2026-11-16: revise (narrow). Folded into revision 3: the dictionary literal design
(`DictEntry` and the extended closed packing protocol, one blocking gap); the false seam claim; the borrowed-address
intrinsic made precise (what the place is, no verifier enforcement, exclusivity site, rvalue keys, `key_kind` error
for non-admitted types); `dict_set` ordering and its visible change; key admission completed (kind-keyed, own-body
exemption, single diagnostics, normative admitted list); staging split (2a, 2b, 3) and wire timing (24 at the switch,
25 at cleanup); first-proposed gates stated now; the discovery set; the debug-scope prerequisite chosen; residual
items corrected (no owned-argument classification, no compiler sendability code). The user answered revision 2's
three open questions on 2026-11-16 (Decisions 3, 4 and 5). Revision 3 was reviewed 2026-11-16: accept with changes. Folded into revision 4: the verifier's literal-protocol
extension specified rule by rule and assigned to stage 5; the interned `DictEntry` and its per-role visibility; `dict_get`
made public (`get`), keyed on the callee's role; the exemption of the type-parameter-key rule inside the declaration;
the explicit amendment of 0025's `-O0` gate and the gate scope; the prototype as a go or no-go verdict; the full
debug-scope prerequisite; wire timing; one per-owner role naming scheme; the consumed-key note for `dict_set`;
authorization wording; characterization additions; the backend layout invariant; the acceptance condition.
Revision 4 has not been reviewed.

## Evidence

The inventory above; prior art 0024, 0025 (the access-mode lowering of the dictionary subscript), 0026 (the array
switch, its measurement method and its lessons).
