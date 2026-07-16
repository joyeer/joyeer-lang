# Joyeer IR — Typed Backend-Neutral Lowering

> **Status:** Core model and JSON-parser MVP lowering implemented and wired
> into `--lang=v0.1`.
> **Input:** `typing::TypeCheckedModel`.
> **Output:** a structurally verified `ir::Module`.

---

## 1. Boundary

Joyeer IR is the replacement frontend's first backend-neutral representation.
It does not reuse legacy `IRGen`, runtime `Type`, bytecode instructions, or VM
objects. The public model is in `include/joyeer/ir/ir.h`; typed AST lowering is
in `include/joyeer/compiler/irlowering.h` and
`lib/compiler/irlowering.cpp`.

A successful `--lang=v0.1` compilation stores the result in
`SourceFile::joyeerIR`. The LLVM/native backend consumes it for
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
storage uses `alloc_stack`, `load`, and `store`. Ordinary parameters enter by
value and are copied to a local slot. `inout` parameters and arguments are
addresses, preserving aliasing instead of silently copying them.
`consuming` parameters enter by value but become owned local storage in the
callee; its normal/early-return cleanup destroys them unless ownership moves
onward.
`initializing` parameters are caller-owned addresses like `inout`, but Stage 5
enforces write-before-read and all-path initialization; they never enter the
callee cleanup stack.

This is intentionally allocation-based rather than SSA. LLVM's `mem2reg` can
promote eligible slots after lowering, while source variables retain simple
and debuggable semantics in the language IR.

---

## 3. Implemented instructions

The current instruction set covers:

- scalar and string constants;
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
- `String`/collection count and value/address subscript operations;
- high-level recursive pattern switching.

`if` expressions merge values through a typed temporary slot. `while` emits a
header, body, exit, and back edge. A `Never` branch terminates without adding a
false fallthrough edge.

Optional promotion is explicit in IR: a type-checked conversion from `T` to
`T?` emits `Optional.Some(T)` at binding, field, argument, arm-merge, or return
boundaries. `nil` emits `Optional.None`.

Heap-backed and recursively nontrivial values have explicit ownership in the
IR. Borrowed values are cloned before entering owned storage; owned temporaries
are moved when possible; overwriting storage destroys the previous value; and
scope exits destroy owned storage and live temporaries in reverse order.
Early returns clean every active scope before transferring the result to the
caller. A typed local declared without an initializer uses `zero_init` when its
type requires destruction, making cleanup safe while Stage 5 rejects any
source-level read before initialization.

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
value, then transfers the replacement value.

---

## 4. Pattern representation

`switch_pattern` is a high-level terminator whose cases retain source order.
Patterns recursively represent:

- wildcard and binding positions;
- integer, Boolean, string, and byte literals;
- enum cases with nested payload patterns.

Each arm has its own basic block. Payload bindings use `extract_payload`, then
a local stack slot. Exhaustiveness is already guaranteed by the type checker;
the future LLVM backend lowers the high-level pattern list into tag and value
tests without reconstructing source semantics.

---

## 5. Verification

`ir::Verifier` rejects:

- duplicate or unknown type/function/block/value identifiers;
- malformed instruction operand/target counts;
- invalid value/address categories;
- load/store, return, operator, call, field, aggregate, or payload type
  mismatches;
- invalid `copy`, `take`, `destroy`, or `zero_init` operand categories/types;
- malformed recursive patterns;
- malformed source maps, out-of-bounds debug spans, or locations without a
  module source map;
- detached/cyclic/cross-function lexical scopes, invalid parameter indices,
  untyped variables, or source locations using the wrong scope;
- missing/duplicate variable bindings, non-address or wrong-typed storage,
  non-dominating declaration storage, and parameter bindings unrelated to the
  incoming parameter;
- blocks without terminators or instructions after a terminator.

Lowering returns an `ir-lowering.verification-failed` diagnostic if generated
IR does not verify. The CLI never advances a malformed module.

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
  backend's explicit Clang optimization level;
- LLVM full-debug emission and variable/type inspection. Backend-neutral
  lexical scopes/source variables, opt-in line tables, CLI policy, and native
  debug artifacts are complete.

None of those should be implemented by extending the compatibility VM lane.
