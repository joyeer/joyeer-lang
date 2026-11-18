# 0025: Capabilities for library-owned collections

- Status: accepted (2026-11-15, revision 3 after two language-designer reviews). Amended by
  [0026](0026-library-array.md): the `-O0` cost gate (0026 Section 4), the `array_literal` method role, and the
  structure `role` enum that replaced the inert-zero bit.
- Spec: §2.6 (`Array`), §4.1.1 (copyability), §4.4 (access paths), §20.12.5 (layout intrinsics) and
  §21 (a library array stays a valid task input) change with this record.
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

1. **First-class projections** (`yield`-style accessor or a returned borrow): a new language feature with
   lifetime rules, excluded by the plan.
2. **A source-visible slot place** that library code forms itself: needs a new access-path root, and the
   unsafe surface grows with no library need that `self[i]`, `move_in` and `move_out` do not cover.
3. **An addressor role with a compiler-internal slot.** The library declares bounds-checked functions
   returning an element's address; only the compiler's lowering of `a[i]` turns that `Int` into a typed
   place. No new source form.

### B. Element-dependent ownership

1. **Conditional conformance** (`extension Array: Copyable where T: Copyable`): not available, because
   `Copyable` and linearity are not protocols here, conformance extensions need the `where` clause 0022
   defers, and linearity is not expressible as conformance.
2. **One type-level role per collection** whose fixed meaning includes the owned type parameters, plus
   method roles for the members the compiler must call.
3. **A zero-sized marker field** (`Owns<T>`) so field propagation sees the owned element: unverified
   against zero-size fields and the live flag, and it still needs separate copy and zero marks. A fallback
   if the role table proves heavier than a marker.
4. **A `deinit` on every array, or an always-linear array:** changes behavior, excluded by the plan.

### C. Linear elements in generic library code

1. **Constant-folded branches** on compile-time queries (`is_linear<T>()`) with the dead arm not lowered.
   Rejected: it needs a new fold in lowering *and* in instance discovery, a checker refinement, and a
   third query for copyability, and a `a && b` or early-`return` shape would silently miss it.
2. **Per-type intrinsics** whose lowering already depends on `T`: a range destroy that traps for a linear
   type, and callback getters that yield `0` when no callback applies.

### D. Layout

1. **No hidden live flag** for a role-bound struct whose destruction is inert on zero storage.
2. **Keep the flag and decouple the C ABI** with a conversion at each native call and at `main(args:)`.

## Decision (proposed)

Option A3, B2, C2 and D1. Nothing below adds source syntax or changes a program that compiles today.

### 1. Roles instead of free attributes

One type-level role per collection, with the 0024 spelling and the same closed, positional validation:
`@compiler_role(array_type)` on `public struct Array<T>` with exactly one type parameter and the stored
fields `address: Int`, `count: Int`, `capacity: Int` in that order (`private`). Its methods live in the
struct body; a prelude `extension` of an `array_type` struct is `invalid-prelude`. The role has a fixed
capability table:

| Capability of `array_type` | Rule |
|---|---|
| **Owns** | the first type parameter: noncopyability, linearity, destruction queries and the verifier's propagation treat the struct as if it held a field of that type |
| **Inert zero** | no hidden `$live` field; the destroy helper runs `deinit` unconditionally, then zero-fills the instance (section 4) |
| **Copy** | copyable exactly when the owned type is, copied by the method with role `array_copy` (section 3); otherwise noncopyable although `deinit` exists |
| **Linear policy** | linear elements allowed (a dictionary role is affine-only; `linear-collection-element` is keyed on this column, not on the public name) |
| **Read-only `count`** | the second field is readable outside the declaration, as a read or a borrow; `&a.count` and any write are rejected |
| **Syntax** (Phase 3) | binds `[T]`, literals, constants, `for-in` |

Method roles, each shape-checked and unique per declaration, with `invalid-prelude` on a bad form:

- `array_element`: `borrowing func ...(index: Int): Int`, an element address, trapping with
  `array index out of bounds` on a bad index, including `data == 0`. For a zero-sized element the address
  is not used (a slot of `Void` is a no-op, as in 0022).
- `array_copy`: `borrowing func ...(): Self`, the deep copy.
- `container_end`: `consuming func ...()`, no parameters, `Void` result (section 5).

### 2. Element access: the `array_element` addressor

For a type with `array_type`, checking keeps `a[i]` as a subscript projection of `a` (wildcard path,
element type is the first type argument), so assignment ordering, the deferred address after the
right-hand side (§4.4.3) and consuming an element are unchanged. Lowering replaces the builtin subscript
by a call of the addressor followed by a **compiler-internal slot place** at the returned `Int`. Only
`lowerAddress`, `prepareAddress` and `isAddressable` gain a role branch: `subscriptAddress` already yields
a typed address that flows into `fieldAddress`, `take`, `destroy` and call arguments, so consume (`take`,
which zero-fills), borrow (the address), mutate (an `inout` argument) and an owned read (a copy, where a
noncopyable `T` is the existing `copy-of-noncopyable`) keep their paths. A subscript on a temporary, a
field or a `borrowing` parameter spills the receiver address for the addressor. Library code reaches
elements only through `self[i]` (lowered this way) or `move_in`, `move_out` and `destroy_at`.

What protects the access:

- The existing statement-level exclusivity analysis (`validateCallExclusivity`, `pathsOverlap`, with a
  subscript as a wildcard step) rejects an overlapping mutation of `a` in the same expression. Lowering
  evaluates index operands, then the right-hand side, then the addressor call, then the access.
- The addressor call is synthesized and is **not itself** subject to call exclusivity; the surrounding
  `a[i]` access carries it.
- The verifier checks type agreement and an escape rule, nothing about reallocation. Library code that
  holds an address across a reallocation is the author's duty, as a malloc'd buffer is in 0005; no new
  safety claim is made.

**IR escape rule.** The result of an `integer_address` is used only in blocks dominated by its definition
(a later argument may split blocks before the call), and only by: a typed load or store address operand,
`take`, `destroy` storage, a `field_address`, `payload_address` or role-subscript derivation, or a call
argument; the rule is transitive through derived addresses. It is never used by `pointer_to_integer`, a
`phi` or select, a return, a stored address value or an aggregate. A malformed-IR test covers each form.
The addressor call is **not** deferred to the call site, which would change the trap order relative to
later arguments' side effects.

**Instance discovery.** Role-lowered uses count as references to the role method instances: `a[i]` (the
addressor), an implicit copy of a concrete copyable `Array<X>` (the `array_copy` instance, found where
lowering inserts the copy, not at a source call site), destruction (`deinit`), `container_end`, and the
element callbacks of section 3 (which need the inner instance).

**Cost mechanism.** The addressor is emitted with LLVM `alwaysinline` and the backend runs the
always-inliner at `-O0` (to be confirmed by a prototype), and the inlined frame keeps the user's debug
locations. The gate is in Consequences.

### 3. Copy and element callbacks: typed intrinsics that depend on `T`

Four layout intrinsics (the family of 0022 and 0023, authorized units only, explicit type arguments):

- `joyeer_intrinsic_clone_at<T>(destination: Int, source: Int)` copies the value at `source` into the
  uninitialized slot `destination` as an implicit copy of a `T` would. Typed once in the generic body; the
  verifier requires a concrete **copyable** `T`. The `array_copy` instance exists only when a copy of
  `Array<T>` is demanded and `T` is copyable, so it never reaches `clone_at` for a noncopyable type.
- `joyeer_intrinsic_destroy_range<T>(address: Int, count: Int)` destroys `count` live elements. For a linear
  `T` it does not destroy: it traps `linear container dropped while not empty` if `count != 0` and does
  nothing otherwise. For a type that needs no destruction it does nothing.
- `joyeer_intrinsic_clone_callback<T>(): Int` and `joyeer_intrinsic_destroy_callback<T>(): Int` return the
  address of the compiler's per-type clone or destroy helper for the array header (the public ABI, native
  `[String]` and constants keep them), or `0` when none applies: always `0` for a noncopyable clone and for
  a linear or trivial destroy. They replace any constant query, so no branch has a dead arm and no
  instance of a linear or noncopyable element ever contains an intrinsic the verifier rejects.

All four are checked once in the generic body, lowered per concrete type, discovered with the callback
instances (the callback address enters instance discovery as in 0023), and rejected outside the prelude.

### 4. Inert zero: destruction without a live flag

The compiler's destroy helper for an `array_type` struct runs `deinit`, then **zero-fills the instance**, so
a second destroy is a no-op without library help. Two facts are stated and tested, not assumed: all-zero
storage is a valid, destroyable state of every `T` (true by construction for the types listed below, since
`take` zero-fills), and the handle `{0, 0, 0}` means no storage, so its `deinit` returns without touching
memory. The tests cover `Int`, `String`, a `deinit` struct, a nested array, an `indirect` enum, a function
value, a `Dict`, optionals of each, and a moved-out slot; any `T` class whose destroy helper does not
tolerate zeros is fixed or excluded before Phase 3 relies on it.

### 5. Linear elements: ending and must-transfer

- **Destroying.** The array `deinit` calls `destroy_range` on its storage and then frees it. For a linear
  owned type that call traps when the array is not empty; the trap is defence in depth, not a language
  guarantee. For other types it destroys each live element.
- **Ending method.** `self` of the `container_end` method is exempt from the linear-not-consumed exit
  check. The compiler destroys it after the body returns on every normal path, running `deinit`, which is
  why `consumeEmpty` traps before returning when `count != 0`; the compiler cannot verify that property,
  exactly as with `arrayConsumeEmpty` today. The text `array not empty` (the current trap) is pinned for
  `consumeEmpty`; a body that returns early instead reaches the `deinit` trap
  `linear container dropped while not empty`, which a test pins separately. The exemption applies to no
  other consuming method of the declaration, which still reports `linear-not-consumed`. It does not reuse
  `stored_consumer` (0023), which means "receiver must be a stored place".
- **Must-transfer bodies.** The 0023 must-transfer rule extends to the `array_type` declaration's bodies, so
  a body cannot drop, copy or implicitly destroy a typed `T`. It cannot see raw memory moves (`copyBytes`
  over live slots, resetting `count`), so a library bug of that kind is caught only by fixtures and the
  `deinit` trap.
- **`consume a[i]` of a linear element** is rejected at the consume site, because it zero-fills a slot that
  can never be ended. Reported by semantic analysis as `semantic-analysis.consume-of-linear-element`. Probed
  against the current compiler (2026-11-15): `let h = consume hs[0]` on `[JoinHandle<Int>]` is accepted, but
  the array can then be neither used (`semantic-analysis.use-after-consume`) nor ended
  (`semantic-analysis.linear-not-consumed`), so no program using it compiles today. The new diagnostic moves
  that error to the consume site; it does not change which programs compile.
- `for x in` over a linear array and a linear `Dict` key or value stay rejected (0015).

## Consequences

- **Compatibility.** No user program, diagnostic or fixture changes, except the possible `consume a[i]`
  diagnostic move above. Users cannot name the roles or intrinsics.
- **Sendability.** An array of `JoinHandle` is a valid task input today (probed: `spawn(function: take,
  input: consume hs)` with `take(value: consuming [JoinHandle<Int>])` compiles and runs). A library
  `Array<T>` owns its elements through an address, so §21's input rule must keep accepting an `array_type`
  value exactly when its owned type is sendable (it is bitwise relocatable). That decision was deferred
  by 0022 to this phase. A fixture pins it before Phase 3; a channel element stays rejected for a linear type.
- **Wire and verifier.** Wire version 19 becomes 20: role metadata for the type and its three methods, the
  inert-zero bit, and the four intrinsics. The verifier rule that a `deinit` struct ends with the live
  flag learns the inert bit. Round-trip and malformed-IR tests are required.
- **Behavior-preservation checks** (each pinned by a test before Phase 3): every existing array diagnostic
  as a golden, `noncopyableReason` wording, `linear-collection-element` keyed on the role, evaluation order
  (index operands, right-hand side, then the bounds trap), the trap text (the location is not pinned),
  zero-sized `[Void]` elements and `data == 0`, overwrite and partial-consume diagnostics.
- **Cost gate** (measured in Phase 3, both profiles, against the unchanged executables, using 0023's paired
  bootstrap): no regression outside noise for `a[i]` read, write and nested access and for `count`; the
  `hello` executable and per-instance code size do not grow beyond a stated bound; `--emit-llvm` for these
  accesses is compared after type-id normalization (the Phase 1 method) as a diagnostic. The
  address-to-pointer round trip can cost alias information at `-O2`, and the array header must still carry
  clone and destroy callbacks, so per-type code exists in both forms; both are what the gate measures.
- **Dictionary (Phase 4).** Role lowering selects an addressor by **access mode**: read, borrow, consume,
  mutate, assign-final. An array uses one addressor for all modes. A dictionary needs two: a lookup that
  traps on a missing key (read, borrow, consume, mutate and any projection base such as `&d[k].f = x`) and an
  inserting addressor used only for the final target of a plain `=`; `&d[k] += 1` does not insert. The insert
  addressor takes an `inout` receiver and an owned key, so role shape validation is per role. Its role owns
  two type parameters and is affine-only, in its capability-table row. Key equality (plan 0012) and the entry
  layout are Phase 4. A test pins that an RHS trap before the insert inserts nothing.

### Tests (written first)

- Roles: each malformed shape, a duplicate, a misplaced method role (a non-consuming `container_end`, a
  labelled `array_element` parameter), an `extension` of the role type, and use outside the prelude, through
  the role-prelude seam of 0024.
- Access: `a[i]` read, borrow, mutate and consume through a test library array; `&a[i] = a.count`,
  `&a[i] += grow(&a)`, nested `a[i][j]`, and `&a.append(a[0])` (rejected today with
  `type-checking.overlapping-access`; pin that); mutation during a
  borrow; the deferred trap order, including a later argument that splits blocks (`f(x: a[i], y: if c ...)`)
  and an argument containing `?`; a subscript on a temporary, a field and a `borrowing` parameter.
- Ownership: a test container whose owned type is linear, noncopyable and copyable; drop on `?`, early
  `return`, overwrite, a dropped temporary and a trap; `container_end` with an early return; the `deinit`
  trap when not empty; a linear container in a struct field and in `Optional<Array<JoinHandle<Int>>>`;
  recursive owns cycles (`struct S<T> { var xs: Array<S<[T]>> }`, terminating); expanding generic recursion;
  an `array_copy` that never instantiates for `Array<Handle>`; a copy reached only through a nested type and
  through a protocol witness.
- Moved-out slots and zero states: `consume a[i]` of copyable, noncopyable and linear elements, then destroy
  and drain; a moved-from array in a struct field; double destruction after `consumeEmpty`; zero-fill after
  `deinit` on the overwrite path; every zero-state `T` class of section 4.
- Callbacks: `destroy_range`, `clone_callback` and `destroy_callback` for linear, noncopyable, trivial and
  nontrivial `T`.
- IR and backend: each escape case, the cross-block case, wire round trip, the layout without the live flag,
  and a C ABI size and offset test on Windows and Linux.
- E2E: a library-written test array exercising append, access, removal, a moved-out slot and drain; an array
  of `JoinHandle` passed to a task and ended there.

## Open questions

1. Whether the backend's `-O0` pipeline runs the always-inliner (prototype; a compiler-emitted check is the
   fallback if the gate is not met). Settled during implementation.
2. Whether `Owns<T>` (B3) would be cheaper than the role table once zero-size fields are checked. Only if the
   role table proves heavy.
3. How the spec spells the read-only `count` exception. The mechanism is decided.
4. **Constant arrays (Phase 3).** A library array's constant data needs a stated plan: the block's destroy,
   the `capacity` convention and the clone at the spawn boundary (§21.2.2). Not a blocker for the capabilities.

## Review

Revision 1 (three free attributes, a source-visible slot) was reviewed 2026-11-15 and changes were
requested: the linear destroy and ending story contradicted 0022's `destroy_at` rule and 0015's exit check;
the copy method could not be written; the owner-pin claim was misattributed and the IR escape rule was
undefined; the inert-zero contract lacked idempotence. Revision 2 (one `array_type` role, method roles, an
internal slot, `clone_at`, a zero-filling destroy helper) was reviewed and accepted with changes. Folded into
revision 3: per-type intrinsics replace the constant-folded dead arm (which did not reach instance
discovery, the checker or the header callbacks); a precise `container_end` rule and pinned trap texts; a
dominance-based IR escape rule; instance discovery of role-lowered uses; access-mode addressors for the
dictionary; the `consume a[i]` diagnostic; the wire and verifier consequences; and the larger test list. The
reviews approve a design, not an implementation.

## Evidence

Inventory of the current implementation recorded in the progress log of plan 0011 (2026-11-15). Prior art:
0022's layout intrinsics and its `borrow_at` gap, 0023's marked prelude contracts and must-transfer
parameters, 0024's roles, closed validation and test seam.
