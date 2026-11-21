# Joyeer IR — Typed Backend-Neutral Lowering

> **Status:** The current typed model and lowering are implemented for the
> [supported language surface](supported-features.md).
> **Input:** `typing::TypeCheckedModel`.
> **Output:** a structurally verified `ir::Module`.

---

## 1. Boundary

Joyeer IR is the compiler's backend-neutral representation. Typed AST lowering
is in `compiler/ir/irlowering.h` and `compiler/ir/irlowering.cpp`. The public
model is split by concern; `compiler/ir/ir.h` includes all of it:

| Header | Contents |
|---|---|
| `value.h` | IDs and typed SSA values |
| `opcode.h` | the instruction set (dense wire values), one descriptor row per opcode, and opcode queries |
| `debuginfo.h` | source tables, debug scopes, variables and locations |
| `definitions.h` | the type table, structs, enums, their roles, constants |
| `module.h` | instructions, blocks, functions and the module |
| `destruction.h`, `dominance.h` | the analyses the verifier and backend share |
| `verifier.h` | the structural verifier; implementation in `verifier/` |
| `dump.h` | deterministic text dumps |

The verifier implementation in `compiler/ir/verifier/` starts with
`internal.h`, which describes the order of its phases and which file holds
each one.

A successful compilation stores the result in `SourceFile::joyeerIR`. The
LLVM/native backend consumes it for
`--emit-llvm`, `-o`, and `--emit-runtime-object`; see the
[compiler backend](backend.md).

Named compilation lowers the entire reachable graph into one IR module.
Each source-language module is an explicit source-file set with its own
logical identity and visibility boundaries; sharing an `ir::Module` for code
generation does not merge those units. Directories do not define IR or
source-language modules. The semantic model's aggregate root contains
declarations from every reachable graph file; it does not concatenate source
text or relocate byte offsets.
`Lowerer::lower(model, sourceName, sourceInfo, sources)` accepts the graph's
source table as its fourth argument. The table order must match the
`SourceSpan::sourceId` values assigned by the frontend.

---

## 2. Representation

The module owns:

- a deterministic snapshot of canonical compile-time `TypeId` names;
- concrete user `struct` field definitions, and for a struct that declares `deinit` the
  function that runs it plus a hidden trailing `$live` Bool field (below), except a struct bound to the library
  array role ([0025](../decisions/0025-library-collection-capabilities.md)), which carries `StructureRole::array`
  (dumped as `role=array`), has no
  live flag, and records its `copier` function when the instance is copyable. A struct bound to the dictionary
  role ([0027](../decisions/0027-library-dict.md)) carries `StructureRole::dict` (`role=dict`) with the same
  inert-zero rules over two owned type arguments, its key and value; the entry a dictionary literal packs carries
  `StructureRole::dictEntry` (`role=dict_entry`): exactly the fields `key` and `value` of its two type arguments, no
  `deinit` and no copier, and it is never a value (its fields are addressed only by verified literal storage);
- user enum and instantiated generic enum case definitions, including the `Optional<T>` / `Result<T,E>`
  instances, whose `EnumerationDefinition` carries the role (`optional` or `result`) and the indices of
  its present/absent or success/failure cases ([0024](../decisions/0024-library-optional-and-result.md)).
  The text dump prints it as a suffix, ` role=optional(0,1)`; a result is always `role=result(0,1)`,
  because lowering binds success to case 0 and failure to case 1 and the verifier requires exactly that;
- external built-ins and source functions;
- functions containing typed values, stack addresses, basic blocks, and
  source-spanned instructions;
- an optional immutable source-file table (file name, directory, byte length,
  and UTF-8 byte offsets for line starts) plus function/instruction debug
  locations.

Functions, types, fields, and enum cases retain graph-wide numeric identities.
Source names are display names, not lookup or linkage keys, so distinct
module declarations and file-private declarations may have identical names.
Qualified calls, constructors, and enum cases lower from their resolved
symbols without evaluating module-name prefixes. `Function::isRootModule`
records whether a function can be considered for executable entry selection;
dependency declarations named `main` keep their source name.

Restricted [C bindings](c-interop.md) add a `NativeBinding` record and
per-function runtime context. Imports remain external declarations without
invented bodies/debug scopes; exports keep normal Joyeer definitions plus
explicit C linkage metadata. The shared verifier validates the complete
scalar ABI, declaration/call authorization, and native representation agreement.
Integer constants retain all 64 payload bits; floating constants retain their
decimal spelling for direct target-precision rounding. Constant types/ranges
and floating pattern literals are verified before LLVM construction.
`Module::isRuntimeObject` requests isolated runtime-object emission without
application startup; its defined root functions require runtime context.

Debug locations are optional so hand-built/backend-only modules remain valid
without source metadata and byte offset zero remains distinguishable from no
location. Compiler-generated parameter plumbing and ownership cleanup retain a
source anchor but are marked implicit, allowing a backend to avoid misleading
source-level stepping stops.

`DestructionAnalysis` snapshots destruction requirements once per type graph,
including nested aggregate payloads. Typed AST lowering, verification, and
LLVM emission each reuse their own snapshot instead of rebuilding type
indexes for every ownership operation. Type definitions must be finalized
before constructing it; changing them requires a fresh analysis. Function
body construction does not invalidate the snapshot, and no cache crosses a
module or the backend C ABI.

Source-map offsets index the exact binary-preserved `SourceFile::content`
buffer for the selected file. `Module::sourceFiles` is indexed directly by
`SourceSpan::sourceId`. A nonempty table is authoritative; when it is empty,
the legacy optional `Module::sourceInfo` supplies only source ID zero.
Function, instruction, lexical-scope, variable, and type-declaration spans
retain their source IDs and file-local offsets. File name and directory are
stored as UTF-8 strings; spans and line starts remain 32-bit, so the verifier
rejects larger source maps before a backend can observe truncated coordinates.

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

### Generic function instances

The frontend checks each generic body once and caches concrete instances
([decision 0021](../decisions/0021-protocols-and-generic-functions.md)).
Lowering emits one function per instance, not a function for the generic
declaration itself. It reads the same syntax with an instance substitution
and resolves each protocol requirement to the concrete conformance's
witness. Direct calls therefore remain ordinary `call` instructions.

Every emitted type is concrete. Generic-only type-table entries become
unusable error placeholders so canonical IDs stay stable; the verifier
rejects an actual `typeParameter` record even when unused. Concrete types
are interned before lowering snapshots the table.
The verifier also rejects error-placeholder types, or types transitively
containing them, in executable values, signatures, aggregate definitions,
constants and debug variables; only unused placeholders may remain.

Each instance has its own function/debug scopes and concrete local types;
instruction spans still point to the generic source. Its display name,
such as `totalArea<Square>`, uses the existing function `name` wire field.
No wire layout or version change is needed. Disambiguating an instance's
type arguments does not rename ordinary nominal types in debug information.

Copy-versus-move decisions use the checked body's unsubstituted types.
`prepareOwned` additionally rejects a resource-owning copy at a span that
was noncopyable in that body. This lowering backstop does not check trivial
values such as `Int`: they need no ownership instruction, and source-level
copy restrictions are enforced by type checking.

---

## 3. Implemented instructions

The current instruction set covers:

- `literal` (integer, byte, Boolean and unit values), floating and String constants;
- stack allocation, zero initialization, load, and store;
- explicit `copy`, `take`, and `destroy` ownership operations;
- implemented arithmetic (`add`, `sub` and `mul` take integer or floating operands), `compare` (scalar operands, a
  Bool result) and bitwise operations;
- checked `div` / `rem` with exactly two `Int` value operands and an `Int`
  value result;
- direct source and external calls, including the external builtins (`print`, byte conversions) and native bindings;
- function values ([decision 0020](../decisions/0020-function-values.md)): `function_address` names a
  Joyeer function and yields it as a value of its function type, and `call_indirect` calls operand 0, a
  function value, with the other operands passed by the type's parameter effects (an address for
  `inout` and `initializing`, a value otherwise). A function type's `TypeName` carries one effect per
  parameter, with the parameters and then the result in `arguments`. The verifier checks the callee's
  exact type, the operand count and categories, the result, and that a call (direct or indirect)
  returning `Never` is followed by `unreachable`. The backend lowers a function type to `ptr` and the call
  to one indirect call whose LLVM type comes from the same helper as every function definition;
- layout intrinsics ([decision 0022](../decisions/0022-generic-types-and-layout-intrinsics.md)): `type_size` and
  `type_alignment` yield an `Int` for the type in `integerValue`, as constant expressions the target's data layout
  folds (the portable module has no layout), and `integer_address` turns an `Int` into the address of a slot of the
  result's type. `move_in`, `move_out` and `destroy_at` lower to `store`, `take` and `destroy` on that address. The
  runtime-only stack slots use `stack_allocate` and `pointer_to_integer`; specialized callback
  addresses use `function_address` and `pointer_to_integer`. The destruction query folds through
  the existing IR destruction analysis;
- collection capabilities ([decision 0025](../decisions/0025-library-collection-capabilities.md)), runtime-only:
  `destroy_range` (address, count) destroys live elements of the type in `integerValue`, `clone_at` copies one
  value into an uninitialized slot, and `type_helper clone` and `type_helper destroy` yield the type's helper
  address as an `Int`. For a linear type, lowering emits `compare ne count, 0` and a runtime-only `trap_if` with the
  message `linear container dropped while not empty` instead of `destroy_range`, and an integer `literal 0` instead
  of a helper that does not apply. A function marked `elementAddressor` is the `array_element` method of a library
  array instance; `a[i]` on such an array lowers to a call of it followed by `integer_address`, the one place
  user code may form an integer address. The verifier requires the `integer_address` of such an element to be used only to load,
  store, take, destroy, project or pass as an argument, and only where its definition dominates. The backend emits
  an element addressor with `alwaysinline` ([0026](../decisions/0026-library-array.md));
- `slot_address` (operands base and index, the element type in `integerValue`, an `Int` result): `base + index *
  size` with no overflow or bounds check, a plain (not `inbounds`) GEP in the backend, `base` for a zero-size type;
  runtime-only. A library addressor uses it after its own bounds check;
- `stack_block` (operand: a constant count, the element type in `integerValue`, an `Int` address result): contiguous stack
  storage for that many slots, allocated in the prologue (once per invocation) and never destroyed; 0 for a zero count or
  a unit type. The verifier requires the operand to be an integer `literal`. Source intrinsics are runtime-only;
  compiler-generated literal storage in user functions follows the closed protocol below;
- unconditional/conditional branches, returns, and unreachable;
- struct construction, field address, and field extraction;
- enum construction, payload extraction (`extract_payload`), and payload addresses
  (`payload_address`, which a moving `match` binding takes from);
- ordinary source calls for library array methods ([0026](../decisions/0026-library-array.md)):
  append owns its element, removal returns an owned optional, and ending consumes the receiver.
  There is no array type kind or array-specific executable opcode;
- ordinary source calls for task/channel APIs ([0023](../decisions/0023-library-concurrency.md)).
  Their concrete structs, `deinit` methods, result construction, raw typed moves and callback
  references use the same operations as other library code. There are no concurrency opcodes;
- ordinary source calls for library dictionary methods ([0027](../decisions/0027-library-dict.md)): `d[k]` calls
  the `dict_element` addressor, `&d[k] = v` calls `dict_set` with an owned key and value, and `get` is the public
  `dict_get` method. There is no dictionary type kind or dictionary-specific opcode: wire 25 removed
  `construct_dictionary`, `dictionary_set`, `dictionary_get`, `subscript_addr` and type kind 11, and the verifier
  rejects that type kind;
- ordinary calls for String operations ([0028](../decisions/0028-ir-opcode-set.md), stage 3): `s.count`, `s[i]`
  (a value; a byte is never addressed), `+` and `+=`, `==`, and the orderings call private, always-inlined functions
  of the collections prelude with closed `string_*` roles; `!=` is `compare eq` of the equality with `false`, and an
  ordering compares the -1, 0 or 1 order with 0. Lowering collects each of these functions only when a String
  operation first calls one and lowers them after every other function, so a module without String operations, such
  as a runtime core unit, has none. Their parameters are read as values, with no stack slot or debug variable, so an
  inlined call keeps no spill at -O0. `s.utf8()` calls the library array's `string_utf8` method on an inert zero
  `[UInt8]`, which is instantiated for no other array. There is no String opcode; String keeps `string` constants,
  literal patterns (whose test still calls the runtime's equality inside the backend) and native ABI marshalling;
- high-level recursive pattern switching.

An array literal prepares all owned elements first, then moves them into one `stack_block` and calls the
private role method marked `literalFactory` on a zero receiver. No element operation can fail during packing.
The verifier admits user-function `stack_block` / `slot_address` / `integer_address` only for this closed graph:
one matching factory call, equal element type and constant count, an inert-zero receiver, every bounded slot
stored exactly once before transfer, and no other uses or intervening calls. A factory call without verified
storage is rejected. This does not grant source-level runtime authorization. Unlike the dictionary protocol below,
it does not check that a stored element value is used once, so hand-written IR could store one owned value into two
slots; lowering never does.

A dictionary literal ([0027](../decisions/0027-library-dict.md), Decision 3) packs into a block of the dictionary's
`dict_entry` instance, whose ordinary layout is the runtime's entry layout. The verifier checks its closed protocol
rule by rule and names the first rule that fails: (1) the literal method gets the receiver, the block and its count;
(2) the element is the entry over the dictionary's key and value; (3) each bounded constant slot has one entry place,
projected once to each of the `key` and `value` fields inside the packing window; (4) each field address has exactly
one use, a store before the transfer of a value of the field's type evaluated before the storage; (5) the count is a
constant and every key is stored, and every value unless `V` is `Void`, when no value field is projected; (6) one
transfer on an inert zero receiver, and only constants, slots, entry places, field addresses and stores in the
window, where each stored value is used once. Lowering emits exactly this form for every dictionary literal.
"Used once" follows paths from the packing block in both directions; the paths that avoid a value's defining block
come from a `DominanceAnalysis` rooted at the packing block (one per direction and literal), not from a walk per
value, so the check stays linear in the function's blocks for each literal.
The wire also preserves the generic `alwaysInline` policy on closed library wrappers; only
authorized functions with bodies may carry it.
Literal factories cannot be formed as function values, so indirect calls cannot evade the storage protocol.
Proven literal blocks use the same protocol in runtime and ordinary functions; other runtime raw blocks
retain their existing capability rules. Wire decoding checks full-width enum and unsigned values before narrowing.
The removed array and dictionary type kinds' slots 10 and 11 stay reserved and are rejected. Opcode values are
dense and follow the descriptor table ([0028](../decisions/0028-ir-opcode-set.md), stage 4, wire 28): the verifier
rejects a value outside the table, and adding, removing or reordering an opcode bumps the wire version. Wire 29
removed `string_utf8`, `count` and `subscript` (stage 3). Wire 27
merged opcode families (0028, stage 2): `literal` replaces the integer, Boolean, byte and unit constants, `compare`
the six comparisons, `convert` the checked and wrapping conversions, `ret` both returns, and `type_helper` the clone
and destroy callbacks; `trap_if` replaces the linear range guard. The predicate, mode or helper kind is the
instruction's `modifier`, whose range the verifier checks from the opcode's row; it is 0 for every other opcode.

A call whose checked argument has type `Never` cannot commit and does not demand a generic callee instance.
Source argument and constraint checks still run. Lowering uses its substituted typed parameter/effect
descriptions to evaluate earlier operands and retain their cleanup, then stops at divergence without a phantom call.

A `literal` of the builtin `Void` type holding 0 is the unit value, with no
operands or resource ownership. Unit patterns lower to typed wildcard
patterns after the frontend checks their exact type. Unit values remain
explicit in enum operands and initialization/consumption analysis, even
though they carry no data bytes.

Calls returning `Void` retain the no-result IR call convention and are
followed by a unit literal for source value contexts. Explicit `return ()`
and `return aVoidReturningCall()` lower to a `ret` with no operand after evaluating the
expression and cleaning active scopes. An early return inside the expression
does not create a second return or a false fallthrough value.

`if` and `match` expressions merge values through a typed temporary slot, which
the enclosing scope destroys. One whose value is discarded
(`TypeCheckedModel::isValueDiscarded`, spec 4.1.1) gets no slot: each branch's
value is discarded in that branch, so a fresh branch temporary is destroyed when
its branch or arm ends, and a branch that names a place does not copy it. `while` emits a
header, body, exit, and back edge. A `Never` branch terminates without adding a
false fallthrough edge. `break` branches to the nearest loop exit and `continue`
to its header. Both emit cleanup only for scopes deeper than the loop's saved
scope depth, including pending expression temporaries and match bindings.
The jump keeps its explicit source location; generated cleanup is implicit.

`==` and `!=` ([0029](../decisions/0029-equatable.md)) on a builtin conformer lower as before: the left operand
is read as a value before the right is evaluated, then `compare` (or the `String` prelude call), and `Void`
equality is a `literal` after both operands. In a generic body the left operand stays borrowed, as the checked
body decided. On a hand-written conformance, `==` is a `call` of the witness (or of its instance for a generic
type, which type checking requires like a call of `equals`) with both operands borrowed, and `!=` adds `compare
eq` with `false`. `t.equals(other: u)` in an instance whose `T` is a builtin conformer is the builtin `==`. A derived
conformance has a generated function `Type.equals(self, other)` per concrete type the program compares (found
with the instances): a struct's `extractField`s each stored field (not the hidden live flag) of both operands
and compares them in declaration order, an enum's `switchPattern`s on both tags and `extractPayload`s the
payloads of equal tags; each `conditionalBranch`es to a shared `false` return at the first difference, and
`Void` and `Never` members are skipped. Its instructions are located at the type's declaration in the
function's root scope. No opcode is added.

`&&` and `||` emit conditional right-hand blocks and Boolean merge slots.
Their right operands and temporaries are evaluated only on the true-left and
false-left paths respectively. Prefix `!` lowers to Boolean equality with
`false`, without adding an eager logical operation. Calls returning `Never`
terminate their IR block with `unreachable`, including when nested under
negation or on either side of a short-circuit expression.

Postfix `?` evaluates its `Result`/`Optional` operand once, then lowers to an
enum-tag switch with success, failure, and continuation blocks. The success
payload moves into the expression result; the failure block constructs the
enclosing function's `.Err` or `.None` and uses the existing return cleanup
path. An owned operand is transferred rather than cloned; a borrowed one
receives the independent owned copy required by value semantics. Typed
payload extraction follows the tag branch, and `Void` payloads carry no
bytes. A `Never` payload has no value to extract, so its impossible branch
terminates as unreachable. No new runtime operation or special IR opcode is
required.

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
this ordering keeps self-assignment valid. For `=` and `op=`, index operands of
the target are lowered first, then the right-hand side, and only then is the
element address computed (an addressor call, `field_addr`): a right side that
reallocates the target's storage cannot leave a stale address behind.
Scope exits destroy owned storage
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

The library append call consumes its element operand. Lowering moves an owned temporary
or clones a borrowed nontrivial value before the call, so runtime
reallocation never aliases the caller's retained value and array destruction
owns every appended element exactly once.

The library dictionary's `dict_set` method likewise consumes a concrete key/value pair. Runtime lookup
decides whether to insert or update: insertion transfers both into a new entry;
update keeps the existing key, destroys the incoming duplicate key and old
value, then transfers the replacement value. A dictionary literal applies
the same last-value-wins policy and counts only unique keys.

`get` is the library's `dict_get` method: an ordinary call that borrows the receiver and the key, performs one
lookup and clones only a present value into `.Some`; absence produces `.None` without a payload copy.

---

## 4. Pattern representation

`switch_pattern` is a high-level terminator whose cases retain source order.
Patterns recursively represent:

- wildcard and binding positions;
- integer, floating, Boolean, string, and byte literals;
- enum cases with nested payload patterns.

Each arm has its own basic block. Payload bindings use `extract_payload`, then
a local stack slot. Nontrivial bindings own independent copies, registered for
cleanup on both normal arm exit and early return. Reassigning the original
scrutinee cannot invalidate a binding, including nested heap-backed payloads.
A pattern that binds a noncopyable payload cannot copy it: the scrutinee is stored (the place of
`match consume x`, or a slot that takes over an owned temporary), the arm takes the payload with
`payload_address` and `take`, and whatever no arm binds is destroyed with that storage.

Every pattern has the type of the value it tests: the scrutinee, or an enum payload. Its kind must
be one the backend can test on that type, or the verifier reports `ir.type-mismatch`:

| Pattern kind | Type | Value |
|---|---|---|
| wildcard | any type | none |
| integer literal | a signed or unsigned integer type, `UInt8` included | fits the type's width (unsigned values as their two's-complement bits) |
| Boolean literal | `Bool` | 0 or 1; the backend keeps only the low bit |
| string literal | `String` | any text |
| byte literal | `UInt8` | 0 to 255 |
| floating literal | `Float` or `Double` | finite text that parses as that type |
| enum case | an enumeration with that case | one payload pattern per case payload |

The first matching case wins, and the case list obeys two rules the verifier enforces:

- **A top-level wildcard is the last case** (`ir.unreachable-switch-case`). A wildcard or
  binding arm matches every value, so `match` lowering ends the dispatch at the first such arm
  and does not lower the arms after it; type checking has already checked them. A nested
  wildcard payload may be followed by further cases, and a case that earlier cases already
  cover is allowed.
- **The cases cover every value of the scrutinee** (`ir.non-exhaustive-switch`). Coverage uses
  the type checker's pattern-matrix algorithm over the IR patterns: a wildcard covers its
  position, `true` and `false` together cover `Bool`, and one case per enum case covers the enum
  when, for each case, the patterns of that case cover its payloads the same way. Integer,
  floating, string and byte literals never cover their type, so a switch over one ends with a
  wildcard. A case whose payload is uninhabited (`Never?`'s `.Some`) still needs a pattern, and
  only a wildcard covers an enum without cases. These conservative rules, which the type
  checker shares, are the only rejections without a missed value. The check has a fixed work
  budget and depth; past either it treats the rest as covered and trusts the producer, so a
  switch is rejected otherwise only when the check found a value that no case matches. The type
  checker runs the same algorithm on the same patterns (a `nil` pattern is `.None` in both, and
  both visit enum cases in declaration order) with a smaller budget, so the verifier settles
  every lowered `match`; its budget only guards other producers. The `?` propagation switch
  names both role cases.

The LLVM backend lowers the high-level pattern list into tag and value tests in order. It
branches to the first wildcard without testing further cases, and ends the tests with
`unreachable`. That is dead for every switch the coverage check settled; past its budget the
verifier trusts the producer, as it does for a type-checked `match`. A producer bug past the
budget is therefore undefined behaviour: at `-O2` LLVM uses the `unreachable` to drop the last
test, so an unmatched value runs the last tested case. Ending the tests with a call to the
runtime's `no switch case matched` panic instead would catch that bug, but it is not free at
`-O2` for a covered switch without a wildcard (measured on x86-64 Windows with LLVM 22.1.8):
LLVM can no longer drop the last tag test, so an enum switch keeps one more compare and a cold
panic block, a function that needed no stack frame gets one for the call, a branch-free `cmov`
selection becomes branches, and the function loses `willreturn` (and `nounwind`, since the
backend does not declare `joyeer_panic` `nounwind`). A `Bool` switch, whose `i1` has no third
value, and a switch that ends with a wildcard compile the same either way. The backend
therefore keeps `unreachable`. It branches on a matching tag before
interpreting payloads, and recursively short-circuits payload tests. Case lookup uses concrete enum type identity
plus case symbol, including distinct builtin container instantiations.

---

## 5. Verification

`ir::Verifier` implements checks for:

- duplicate or unknown type/function/block/value identifiers;
- unsubstituted generic type parameters, including unused type records;
- executable uses of generic/error placeholders, including nested type arguments;
- unknown or removed opcodes, and operand/target counts or results that do not match the opcode's row;
- invalid value/address categories;
- selected load/store, return, operator, call, field, aggregate, and payload
  type relationships; opcode-specific Boolean constraints remain incomplete;
- invalid `copy`, `take`, `destroy`, or `zero_init` operand categories/types;
- a structure `deinit` that is not a function taking only the structure's address and returning
  nothing, or a struct with one that does not end with the hidden live flag (an array-role struct must not
  have one, and must have a `deinit`); a `copier` that is not a function borrowing the struct and returning it;
  an element address that is converted, returned, stored as a value, or used outside its dominance; a
  collection opcode outside a runtime function, or `clone_at` on a noncopyable type;
- malformed recursive patterns, a pattern kind or literal value its type cannot hold, and
  `switch_pattern` cases after a top-level wildcard or that miss a value of the scrutinee
  (section 4);
- malformed source maps, unknown source IDs, debug spans outside their own
  file's byte bounds, or locations without module source information;
- detached/cyclic/cross-function lexical scopes, invalid parameter indices,
  untyped variables, or source locations using the wrong scope;
- missing/duplicate debug-variable bindings, non-address or wrong-typed
  storage, non-dominating debug declaration storage, and parameter bindings
  unrelated to the incoming parameter;
- blocks without terminators or instructions after a terminator.

Every opcode has one `OpcodeDescriptor` row in `opcode.cpp`
([0028](../decisions/0028-ir-opcode-set.md), stage 1): its spelling, operand count (fixed, or variadic when its own
check matches the operands against a callee, a function type or a definition), target count, result rule, immediate
kind (what `integerValue` or `text` holds), whether it ends its block, and whether it is runtime-only. Before an
opcode's own check, the verifier rejects a value outside the table, checks a fixed operand and target count and the result
rule once, and marks a runtime-only opcode outside a runtime function as unauthorized; the opcode's check reports
that unless it admits verified literal storage or an element place. The dumper prints immediates by kind, so a new
opcode needs a dumper case only for a callee, block targets, switch cases, parenthesized operands, or an immediate
after its operands (as `fits` has).

Lowering returns an `ir-lowering.verification-failed` diagnostic when a modeled
check fails, at the span of the instruction the error names (`ir::verificationErrorSpan`). An
error that names no instruction, such as a duplicate type, function or block identifier or a
block without a terminator, has an empty span. Passing this verifier is not proof of source-level correctness or
validity of all subsequently emitted LLVM text. Native output additionally
parses and verifies that text in the LLVM backend.

General operand dominance is currently missing: hand-built IR can use an
address before its definition or outside its dominating block and still pass
both Joyeer verification and text emission. An `Int` condition for `cond_br`
can likewise pass until LLVM rejects the text. Existing definition locations and `DominanceAnalysis` must be applied to
ordinary operands, and opcode contracts must check concrete `TypeKind` values.

`DominanceAnalysis` answers the verifier's dominance checks. The entry block
and every other block without predecessors root the graph; a block that no
root reaches is treated as dominated by every block. It builds a dominator tree
by the Cooper, Harvey and Kennedy iterative algorithm in reverse postorder and
answers each query by comparing tree intervals. Straight-line branching costs a
bounded number of steps per block; many predecessors joining one block at
increasing depth (a loop with many `break`s) still cost about the square of
their count. The `work()` bound tests in `ir_test.cpp` record both shapes and
a randomized comparison against the set-based fixed point pins the semantics.

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

The lowering suite includes parser acceptance, unit-value, and ownership
fixtures.

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
| Conditional evaluation | `&&` and `||` branch before right-hand evaluation, including side effects, bounds checks, early returns, and loop exits. |
| Pattern payloads | Tag branches guard payload interpretation, including nested enum/String patterns. |
| Pattern binding ownership | Nontrivial bindings own copies independent of the scrutinee and are destroyed on normal exit and early return. |
| Binary operand capture | Nontrivial left operands remain alive across right-hand side effects and are cleaned on divergence. |
| Aggregate capture | Components become ownership-safe in evaluation order, before later mutation can invalidate a borrowed source. |
| Pending operand cleanup | Acquiring ownership does not commit transfer; later operand divergence still cleans prepared values. |
| Loop stack storage | LLVM source and scratch allocations are emitted in a one-time function prologue, not reallocated on every iteration. Initialization and debug-variable bindings remain at their source points. |
| Enum identity | Case lookup includes concrete `TypeId`; distinct `Optional`/`Result` instantiations do not collide, and their role indices come from the definition, never from case names. The verifier rejects a role with a wrong shape, a value outside the closed set, one declaration bound to two roles, or payloads that disagree with the type arguments. Failed emission produces diagnostics and no partial success. |
| Optional binding promotion | Binding initialization applies its destination conversion before storing. |
| `deinit` (decision 0015) | A struct that declares one records its function (a method-like function taking the struct's address) and ends with a hidden `$live` Bool. Construction sets it; `take`, zero-initialization and the end of destroy clear it, so only a live value runs the `deinit`, once, before its fields are destroyed in reverse order. Returning an owning local of a noncopyable type is lowered as a `take`; binding a noncopyable value in a pattern and copying one are lowering errors. |

These are regression-backed implementation rules, not a complete ownership
proof. The structural verifier limitations in section 5 and broader
optimization/ABI work in section 6 remain.
