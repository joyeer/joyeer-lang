# Joyeer IR — Typed Backend-Neutral Lowering

> **Status:** Core model and JSON-parser MVP lowering implemented.
> **Input:** `typing::TypeCheckedModel`.
> **Output:** a structurally verified `ir::Module`.

---

## 1. Boundary

Joyeer IR is the compiler's backend-neutral representation. The public model
is in `include/joyeer/ir/ir.h`; typed AST lowering is in
`include/joyeer/compiler/irlowering.h` and
`lib/compiler/irlowering.cpp`.

A successful compilation stores the result in `SourceFile::joyeerIR`. The
LLVM/native backend consumes it for
`--emit-llvm` and `-o`; see [native.md](native.md).

---

## 2. Representation

The module owns:

- a deterministic snapshot of canonical compile-time `TypeId` names;
- concrete user `struct` field definitions;
- user enum and instantiated `Optional<T>` / `Result<T,E>` case definitions;
- external built-ins and source functions;
- functions containing typed values, stack addresses, basic blocks, and
  source-spanned instructions;
- an optional immutable source map (file name, directory, byte length, and
  UTF-8 byte offsets for line starts) plus function/instruction debug
  locations.

Debug locations are optional so hand-built/backend-only modules remain valid
without source metadata and byte offset zero remains distinguishable from no
location. Compiler-generated parameter plumbing and ownership cleanup retain a
source anchor but are marked implicit, allowing a backend to avoid misleading
source-level stepping stops.

Source-map offsets index the exact binary-preserved `SourceFile::content`
buffer. File name and directory are stored as UTF-8 strings; spans and line
starts remain 32-bit, so the verifier rejects larger source maps before a
backend can observe truncated coordinates.

The IR also snapshots source lexical scopes, parameters, local/pattern
variables, and the exact event where each variable gains stable address
storage. Function, block, and match-arm scopes preserve the semantic hierarchy
independently of ownership-cleanup scopes. Address parameters bind at function
entry; by-value parameters, initialized locals, deferred locals, and pattern
bindings bind only when their storage becomes valid. Diverging initializers do
not create a source variable.

Values and addresses are different `ValueCategory` values. Mutable local
storage uses `alloc_stack`, `load`, and `store`. Ordinary borrowing parameters
enter by value and are shallow-stored in nonowning local slots; this is not a
recursive owned copy. `inout` parameters and arguments are
addresses, preserving aliasing instead of silently copying them.
`consuming` parameters enter by value but become owned local storage in the
callee; its normal/early-return cleanup destroys them unless ownership moves
onward.
`initializing` parameters are caller-owned addresses like `inout`. Stage 5 is
responsible for write-before-read and all-path initialization through its
[initialization data flow](semantic-analysis.md#4-definite-initialization);
these addresses never enter the callee cleanup stack.

This is intentionally allocation-based rather than SSA. LLVM's `mem2reg` can
promote eligible slots after lowering, while source variables retain simple
and debuggable semantics in the language IR.

---

## 3. Implemented instructions

The current instruction set covers:

- scalar, string, and `unit` constants;
- stack allocation, zero initialization, load, and store;
- explicit `copy`, `take`, and `destroy` ownership operations;
- MVP arithmetic, comparison, and logical operations;
- direct source and external calls;
- unconditional/conditional branches, returns, and unreachable;
- struct construction, field address, and field extraction;
- enum construction and payload extraction;
- mutating `array_append` with an addressable receiver and transferred element;
- inserting/updating `dictionary_set` with an addressable receiver and
  transferred key/value;
- `String`/collection count and value/address subscript operations, plus owned
  `String.utf8()` byte-array extraction;
- high-level recursive pattern switching.

The `unit` instruction produces a value of the builtin `Void` type with no
operands or resource ownership. Unit patterns lower to typed wildcard
patterns after the frontend checks their exact type. Unit values remain
explicit in enum operands and initialization/consumption analysis, even
though they carry no data bytes.

Calls returning `Void` retain the no-result IR call convention and are
followed by a unit constant for source value contexts. Explicit `return ()`
and `return aVoidReturningCall()` lower to `ret_void` after evaluating the
expression and cleaning active scopes. An early return inside the expression
does not create a second return or a false fallthrough value.

`if` expressions merge values through a typed temporary slot. `while` emits a
header, body, exit, and back edge. A `Never` branch terminates without adding a
false fallthrough edge. `&&` emits a conditional right-hand block and a Boolean
merge slot; its right operand and temporaries are evaluated only on the
true-left path.

Optional promotion must be explicit in IR: an accepted conversion from `T` to
`T?` needs `Optional.Some(T)` at its value boundary, while `nil` needs
`Optional.None`. Binding initializers, aggregate operands, call arguments,
branch results, and return values apply their destination conversion before
ownership transfer and storage.

Heap-backed and recursively nontrivial values have explicit ownership in the
IR. Borrowed values are cloned before entering owned storage, so ordinary
binding initialization and assignment leave the source usable. Owned
temporaries transfer directly. An overwrite first acquires the owned
replacement, then destroys the previous value, then stores the replacement;
this ordering keeps self-assignment valid. Scope exits destroy owned storage
and live temporaries in reverse order.
Early returns must clean every active scope, including pending temporaries,
before transferring the result to the caller. A typed local declared without
an initializer uses `zero_init` when its type requires destruction, making
cleanup safe without granting permission to read uninitialized source storage.

Operand preparation and transfer are separate. An aggregate component is
converted and made independently owned before the next component is
evaluated. Pending components and consuming call arguments remain registered
for cleanup until the construction or call is emitted, so a later operand's
early return cleans them rather than leaking them.

Non-short-circuit binary expressions also prepare the left operand before
evaluating the right. A borrowed nontrivial operand is copied and registered
as a temporary, so mutation during right-hand evaluation cannot invalidate it.
Early returns clean this temporary through the normal active-scope cleanup.

A `consume` argument backed by local storage emits `take`, which loads and
zeroes the caller slot before the call. Owned temporaries move directly;
borrowed nontrivial values are not valid consuming sources. Field, array, and
dictionary projections lower through their existing address operations, then
`take` zeroes the projected storage. Parent aggregate destruction remains safe,
and a later assignment reinitializes the projection.

`array_append` consumes its element operand. Lowering moves an owned temporary
or clones a borrowed nontrivial value before the instruction, so runtime
reallocation never aliases the caller's retained value and array destruction
owns every appended element exactly once.

`dictionary_set` likewise consumes a concrete key/value pair. Runtime lookup
decides whether to insert or update: insertion transfers both into a new entry;
update keeps the existing key, destroys the incoming duplicate key and old
value, then transfers the replacement value. Dictionary construction applies
the same last-value-wins policy and counts only unique keys.

---

## 4. Pattern representation

`switch_pattern` is a high-level terminator whose cases retain source order.
Patterns recursively represent:

- wildcard and binding positions;
- integer, Boolean, string, and byte literals;
- enum cases with nested payload patterns.

Each arm has its own basic block. Payload bindings use `extract_payload`, then
a local stack slot. Nontrivial bindings own independent copies, registered for
cleanup on both normal arm exit and early return. Reassigning the original
scrutinee cannot invalidate a binding, including nested heap-backed payloads.
The LLVM backend lowers the high-level pattern list into
tag and value tests, relying on the type checker's recursive payload coverage.
It branches on a matching tag before interpreting payloads, and recursively
short-circuits payload tests. Case lookup uses concrete enum type identity
plus case symbol, including distinct builtin container instantiations.

---

## 5. Verification

`ir::Verifier` implements checks for:

- duplicate or unknown type/function/block/value identifiers;
- malformed instruction operand/target counts;
- invalid value/address categories;
- selected load/store, return, operator, call, field, aggregate, and payload
  type relationships; opcode-specific Boolean constraints remain incomplete;
- invalid `copy`, `take`, `destroy`, or `zero_init` operand categories/types;
- malformed recursive patterns;
- malformed source maps, out-of-bounds debug spans, or locations without a
  module source map;
- detached/cyclic/cross-function lexical scopes, invalid parameter indices,
  untyped variables, or source locations using the wrong scope;
- missing/duplicate debug-variable bindings, non-address or wrong-typed
  storage, non-dominating debug declaration storage, and parameter bindings
  unrelated to the incoming parameter;
- blocks without terminators or instructions after a terminator.

Lowering returns an `ir-lowering.verification-failed` diagnostic when a modeled
check fails. Passing this verifier is not proof of source-level correctness or
validity of all subsequently emitted LLVM text. Native output additionally
parses and verifies that text in the LLVM backend.

General operand dominance is currently missing: hand-built IR can use an
address before its definition or outside its dominating block and still pass
both Joyeer verification and text emission. An `Int` condition for `cond_br`,
or an `Int`-typed comparison result, can likewise pass until LLVM rejects the
text. Existing definition locations/dominators must be applied to ordinary
operands, and opcode contracts must check concrete `TypeKind` values.

The verifier is not currently an ownership or definite-initialization proof.
For example, it accepts destruction of shallow-stored borrowed String
storage. Explicit ownership operations make those obligations representable,
but do not by themselves verify them.

The deterministic textual form is produced by `ir::dump` and includes types,
aggregate definitions, signatures, blocks, instructions, symbols, source
spans, and recursive patterns.

Direct validation:

```pwsh
ctest --test-dir build -L "ir|ir-lowering" --output-on-failure
```

The lowering suite includes the complete JSON-parser MVP fixture.

---

## 6. Remaining backend work

The current IR/native pipeline intentionally leaves these to later commits:

- longer-lived exclusivity beyond a single call;
- copy-elision and ABI tuning for large aggregates;
- enum niche optimization and a stable public ABI;
- Joyeer-specific optimization passes and an LTO policy beyond the native
  backend's explicit LLVM optimization level;
- optimized-debug value tracking beyond the current lexical scope, source
  variable, physical type, and `llvm.dbg.declare` metadata.

These belong in Joyeer IR, LLVM lowering, or the native ABI rather than a
second execution pipeline.

## 7. Evaluation and storage invariants

Native regression fixtures exercise these invariants at both `-O0` and `-O2`:

| Area | Implementation |
|---|---|
| Conditional evaluation | `&&` branches before right-hand evaluation, including side effects, bounds checks, and early returns. |
| Pattern payloads | Tag branches guard payload interpretation, including nested enum/String patterns. |
| Pattern binding ownership | Nontrivial bindings own copies independent of the scrutinee and are destroyed on normal exit and early return. |
| Binary operand capture | Nontrivial left operands remain alive across right-hand side effects and are cleaned on divergence. |
| Aggregate capture | Components become ownership-safe in evaluation order, before later mutation can invalidate a borrowed source. |
| Pending operand cleanup | Acquiring ownership does not commit transfer; later operand divergence still cleans prepared values. |
| Loop stack storage | LLVM source and scratch allocations are emitted in a one-time function prologue, not reallocated on every iteration. Initialization and debug-variable bindings remain at their source points. |
| Builtin enum identity | Case lookup includes concrete `TypeId`; distinct `Optional`/`Result` instantiations do not collide. Failed emission produces diagnostics and no partial success. |
| Optional binding promotion | Binding initialization applies its destination conversion before storing. |

These are regression-backed implementation rules, not a complete ownership
proof. The structural verifier limitations in section 5 and broader
optimization/ABI work in section 6 remain.
