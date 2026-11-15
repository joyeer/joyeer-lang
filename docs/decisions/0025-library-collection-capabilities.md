# 0025: Capabilities for library-owned collections

- Status: proposed (2026-11-15, revision 2 after one language-designer review)
- Spec: not yet. On acceptance §2.6 (`Array`), §4.1.1 (copyability), §4.4 (access paths) and §20.12.5
  (layout intrinsics) change; §21 does not (a linear array is not sendable by this record).
- Plan: [0011](../plan/0011-library-containers.md), Phase 2.

## Context

Phase 1 made `Optional` and `Result` ordinary library enums ([0024](0024-library-optional-and-result.md)).
`Array` and `Dict` are next, and plan 0011 Phase 2 asks what the *compiler* must provide so that
`Array<T>` and `Dict<K, V>` can be written as library structs without losing today's behavior.

Their algorithms already live in Joyeer (`lib/Runtime/core/arrays.joyeer`, `dictionaries.joyeer`).
The compiler still owns the type kinds, the 24-byte `{data, count, capacity}` layout, element access
(`a[i]` is an inline bounds check plus a typed address), construction and constant arrays, the per-type
clone and destroy callbacks, and every ownership rule. An inventory of the current code found these
obstacles to a library `struct Array<T>`:

1. **Element access.** There is no user subscript, accessor or projection. Borrows are call-scoped, and
   0022 records the gap: no `borrow_at`, so a library array cannot read an element without moving it.
2. **Copyability.** A struct with `deinit` is always noncopyable, but `[Int]` and `[String]` are
   copyable with a deep clone. Nothing says "copyable if `T` is, copying like this".
3. **Linearity.** An array of `JoinHandle` is linear and ends only through `consumeEmpty()`. A library
   array owns its elements through an address, not a field, so field propagation sees nothing; a linear
   struct may not declare `deinit`; and user generics reject linear arguments.
4. **The hidden `$live` flag.** A `deinit` struct gets a trailing `Bool` that `take` clears (a move
   zero-fills its source) so a moved-from value does not run `deinit`. It makes the layout 32 bytes, which
   breaks the public `JoyeerArray` C ABI, the constant-array encoding and native buffer passing.
5. **Moved-out slots.** `consume a[i]` leaves a zero-filled slot. For a linear element the array can then
   never be ended (`consumeEmpty` traps on a nonzero count, and `&a[i] = h` is `linear-overwrite`).
6. **Cost.** `a[i]` and `count` are inline today, also at `-O0`.

This record decides the compiler capabilities only. It does not move `Array` (Phase 3) or `Dict`
(Phase 4). Every capability is in compiler-bundled prelude sources only and is rejected elsewhere with
the existing unauthorized-use diagnostics, as the roles of 0023 and 0024 are.

## Options considered

### A. Element access

1. **First-class projections** (`yield`-style accessor or a returned borrow). A new language feature with
   lifetime rules; the plan's out-of-scope list excludes it.
2. **A source-visible slot place** (`joyeer_intrinsic_slot<T>(address)`) that library code forms itself.
   Reviewed and rejected: it needs a new access-path root (`pathsOverlap` compares roots, and a call
   expression is not storage today), and the unsafe surface grows with no library need that
   `self[i]`, `move_in` and `move_out` do not cover.
3. **An addressor role with a compiler-internal slot.** The library declares one bounds-checked function
   returning an element's address; only the compiler's lowering of `a[i]` turns that `Int` into a typed
   place. No new source form.

### B. Element-dependent ownership

1. **Conditional conformance** (`extension Array: Copyable where T: Copyable`). Not available: `Copyable`
   and linearity are not protocols here, conformance extensions need the `where` clause 0022 defers, and
   linearity is not expressible as conformance at all.
2. **One type-level role per collection** whose fixed meaning includes "owns these type parameters",
   plus method roles for the few members the compiler must call.
3. **A zero-sized marker field** (`Owns<T>`) so existing field propagation sees the owned element.
   Unverified against zero-size fields and the live flag, and it still needs separate copy and zero
   marks; recorded as a fallback if the role table proves heavier than a marker.
4. **A `deinit` on every array, or an always-linear array.** Rejected in the plan: it changes behavior.

### C. Layout

1. **No hidden live flag** for a role-bound struct whose destruction is inert on zero storage.
2. **Keep the flag and decouple the C ABI** with a conversion at each native call and at `main(args:)`.

## Decision (proposed)

Option A3, B2 and C1. Nothing below adds source syntax or changes a program that compiles today.

### 1. Roles instead of free attributes

One type-level role per collection, with the 0024 spelling and the same closed, positional validation:

`@compiler_role(array_type)` on `public struct Array<T>` with exactly one type parameter and the stored
fields `address: Int`, `count: Int`, `capacity: Int` in that order (`private`). The role has a fixed
table of meaning, so there is one place that says what is closed:

| Meaning of `array_type` | Rule |
|---|---|
| **Owns** the first type parameter | noncopyability, linearity, destruction queries and the verifier's propagation treat the struct as if it held a field of that type |
| **Inert zero** | no hidden `$live` field; the destroy helper runs `deinit` unconditionally and then zero-fills the instance (§4) |
| **Copy** | copyable exactly when the owned type is, copied by the method with role `array_copy` (§3); otherwise noncopyable although `deinit` exists |
| **Read-only `count`** | `a.count` reads the second field from outside the declaration; writing it is rejected |
| **Syntax** (Phase 3) | binds `[T]`, array literals, constants, `for-in` |
| **Linear elements** | allowed (a dictionary role will be affine-only, Phase 4) |

Method roles, each validated for shape and unique per declaration:

- `array_element`: `borrowing func ...(index: Int): Int`, returns an element address and traps (message
  `array index out of bounds`) on a bad index, including `data == 0` and a zero-sized element.
- `array_copy`: `borrowing func ...(): Self`, the deep copy.
- `container_end`: `consuming func ...()` with no parameters and `Void` result, the ending method (§5).

This replaces any new `@compiler_*` attribute: validation is the 0024 mechanism (one recognised attribute,
a closed value, positional binding, `invalid-prelude` on a bad shape).

### 2. Element access: the `array_element` addressor

For a type with `array_type`, checking keeps `a[i]` as a subscript projection of `a` (wildcard path,
element type is the first type argument), so assignment ordering, the deferred address after the
right-hand side (§4.4.3) and consuming an element are unchanged. Lowering replaces the builtin subscript
by a call of the addressor followed by a **compiler-internal slot place** at the returned `Int`. Library
code reaches elements only through `self[i]` (lowered this way) or `move_in`, `move_out` and `destroy_at`.

What protects the access, stated precisely:

- The existing statement-level exclusivity analysis (`validateCallExclusivity`, `pathsOverlap`, with a
  subscript as a wildcard step) rejects an overlapping mutation of `a` in the same expression; lowering
  evaluates index operands, then the right-hand side, then the addressor call, then the access.
- The addressor call is synthesized and is **not itself** subject to call exclusivity; the surrounding
  `a[i]` access carries it.
- The verifier checks type agreement and a local escape rule, nothing about reallocation. Within library
  code nothing stops `self.append(value: self[i])`-style misuse beyond the exclusivity analysis, and the
  library author pairs slots with live values, as with a malloc'd buffer in 0005. No new safety claim is made.

IR escape rule (checkable): an `integer_address` result is used only in its defining block, only by a typed
load, store, `take`, or an argument of a call; never by `pointer_to_integer`, a `phi`, a stored address value,
or a return. Malformed-IR tests cover each.

Cost mechanism: the addressor is emitted with LLVM `alwaysinline` and the backend runs the always-inliner
at `-O0` (to be confirmed by a prototype); debug locations of the inlined frame keep the user span. The
gate is stated in Consequences.

### 3. Copy: `array_copy` and `joyeer_intrinsic_clone_at<T>`

The deep copy needs a typed clone of one element. A new compile-time-typed layout intrinsic,
`joyeer_intrinsic_clone_at<T>(destination: Int, source: Int)`, copies the value at `source` into the
uninitialized slot `destination` exactly as an implicit copy of a `T` would. It is typed once in the
generic body like the other intrinsics; the verifier requires a **concrete copyable** `T`. The copy method is
instantiated only when a copy of `Array<T>` is demanded and the owned type is copyable, so a noncopyable
instance never reaches the intrinsic. Tests: the method never instantiates for `Array<Handle>`, and the
verifier rejects `clone_at` of a noncopyable type in hand-built IR.

### 4. Inert zero: destruction without a live flag

The compiler's destroy helper for an `array_type` struct runs `deinit`, then **zero-fills the instance**
(the library need not, so a second destroy is a no-op). Two facts are stated and tested rather than
assumed: all-zero storage is a valid, destroyable state of every `T` (true by construction: `take` zero-fills
and zero-initialization is the uninitialized state), and the array handle `{0, 0, 0}` means no storage, so its
`deinit` returns without touching memory. The test list covers `Int`, `String`, a `deinit` struct, a nested
array, an optional of each, and a moved-out slot.

### 5. Linear elements: ending and must-transfer

- **No element destroy for a linear type.** The array `deinit` branches on the compile-time queries
  `joyeer_intrinsic_needs_destroy<T>()` and a new `joyeer_intrinsic_is_linear<T>()`. For linear `T` it never
  destroys elements: if `count != 0` it traps `linear container dropped while not empty`, then frees
  storage. For other `T` it destroys each live element with `destroy_at<T>`. The dead arm of a branch on one
  of these two constants is **not lowered**, so `destroy_at<T>` appears only in instances where `T` is not
  linear (which 0022 requires of the verifier). The trap is defence in depth, not a language guarantee.
- **Ending method.** The method with role `container_end` is exempt from `linear-not-consumed` for `self`:
  its body ends `self` explicitly, and the compiler then destroys the (empty) instance, running its `deinit`
  once. The exemption is only for that role, whose shape the compiler checks. It does not reuse
  `stored_consumer` (0023), which means "receiver must be a stored place". `consumeEmpty` traps before any
  effect when `count != 0`; the compiler cannot verify that property, exactly as with `arrayConsumeEmpty`
  today.
- **Must-transfer bodies.** The 0023 must-transfer rule extends to the `array_type` declaration's bodies, so a
  body cannot drop, copy or implicitly destroy a typed `T`. It cannot see raw memory moves (`copyBytes`
  over live slots, resetting `count`), so a library bug of that kind is caught only by fixtures and the
  `deinit` trap.
- **`consume a[i]` of a linear element** is rejected, because it zero-fills a slot that can never be ended.
  Whether it is rejected today is unknown; a fixture comes first, and if it is accepted now the rejection
  is a recorded soundness fix, not part of "no behavior change".
- `for x in` over a linear array and a linear `Dict` key or value stay rejected (0015).

## Consequences

- **Compatibility.** No user program, diagnostic or fixture changes, except the possible `consume a[i]`
  soundness fix above. Users cannot name the roles or intrinsics.
- **Wire and IR.** Wire version 19 becomes 20: role metadata for the type and its three methods, the
  inert-zero bit, and `clone_at`, `is_linear`. Round-trip and malformed-IR tests are required.
- **Behavior-preservation checks** (each pinned by a test before Phase 3): every existing array diagnostic
  as a golden, `noncopyableReason` wording, `linear-collection-element` keyed on the role not the name,
  evaluation order (index operands, right-hand side, then the bounds trap), the trap text and location,
  zero-sized `[Void]` elements and `data == 0`, overwrite and partial-consume diagnostics.
- **Cost gate** (measured in Phase 3, both profiles, against the unchanged executables, using 0023's paired
  bootstrap): no regression outside noise for `a[i]` read, write and nested access and for `count`; the
  `hello` executable and per-instance code size do not grow beyond a stated bound; `--emit-llvm` for these
  accesses is compared after type-id normalization (the Phase 1 method) as a diagnostic. The
  address-to-pointer round trip can cost alias information at `-O2`, and the array header must still carry
  clone and destroy callbacks for the C ABI, native `[String]` and constants, so per-type code exists in both
  forms; both are what the gate measures.
- **Dictionary (Phase 4)** does not reuse `array_element`: `d[k]` traps on a missing key as a read or
  projection base but inserts as the final assignment step, so a dictionary needs two addressors or a mode.
  Its role owns two type parameters and is affine-only, a fact in its role row rather than a dispatch on the
  public name. This record leaves that shape to Phase 4.

### Tests (written first)

- Roles: each malformed shape, a duplicate, a misplaced method role, and use outside the prelude, through
  the role-prelude seam of 0024.
- Access: `a[i]` read, borrow, mutate and consume through a test library array; `&a[i] = a.count`,
  `&a[i] += grow(&a)`, `&a.append(a[0])`, nested `a[i][j]` (pin whatever the checker does today);
  mutation during a borrow; the deferred bounds trap order.
- Ownership: a test container with a linear, a noncopyable and a copyable owned type; drop on `?`, early
  `return`, overwrite, a dropped temporary and a trap; the `deinit` trap when nonempty; a linear container
  in a struct field and in `Optional<Array<JoinHandle<Int>>>`; recursive owns cycles
  (`struct S<T> { var xs: Array<S<[T]>> }`, visiting-set termination); expanding generic recursion.
- Moved-out slots: `consume a[i]` of copyable, noncopyable and linear elements, then destroy and drain.
- IR and backend: each verifier escape case, wire round trip, the layout without the live flag, and a C ABI
  size and offset test on Windows and Linux.
- E2E: a library-written test array exercising append, access, removal, a moved-out slot and drain.

## Open questions

1. Whether the backend's `-O0` pipeline runs the always-inliner (prototype; the record accepts a
   compiler-emitted check as a fallback if it does not meet the gate).
2. Whether `Owns<T>` (B3) would be cheaper than the role table once zero-size fields are checked; settle by
   prototype only if the role table proves heavy.
3. Phase 3 must decide how `a.count` read-only access is spelled in the spec (a role rule here; no computed
   property exists).

## Review

Language-designer review of revision 1 (2026-11-15), requested changes. Folded in: replaced three
free attributes and a source-visible slot with one type role plus method roles and a compiler-internal slot;
a distinct `container_end` role instead of overloading `stored_consumer`; `clone_at` for the copy method;
the linear `deinit` never destroys elements, with the constant-folded dead-arm rule; the destroy helper
zero-fills; an exact statement of what protects an access and a checkable IR escape rule; the `consume a[i]`
dead end; a dictionary extension note and the affine-only flag; a cost gate and a larger test list.

## Evidence

Inventory of the current implementation recorded in the progress log of plan 0011 (2026-11-15). Prior art:
0022's layout intrinsics and its `borrow_at` gap, 0023's marked prelude contracts and must-transfer
parameters, 0024's roles, closed validation and test seam.
