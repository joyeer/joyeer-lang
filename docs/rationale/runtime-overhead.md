# Joyeer Runtime Cost Goals and Current Baseline

> **Status:** cost rationale and measurement goals, not a delivered size or
> performance guarantee. The normative language rules are in
> [spec.md](../spec.md); current representations and lowering are documented in
> [native.md](../impl/native.md) and [ir.md](../impl/ir.md).

Joyeer aims to make the cost of a feature comparable to a careful native
implementation of the same semantics. That comparison must include ownership,
copying, bounds checks, overflow behavior, error handling, and platform startup
requirements. Comparing checked Joyeer operations with unchecked C operations
does not establish abstraction overhead.

## 1. What zero-cost means here

The design goals are:

- no GC or reference counting;
- no avoidable work for unused language abstractions;
- predictable costs for concrete value operations;
- deterministic destruction on normal scope exit and return;
- opportunities to remove redundant copies and checks without changing
  source validity or observable behavior.

These goals do not mean every source operation is free, every copy is constant
time, or runtime checks disappear in optimized builds. The current compiler
has not demonstrated C/C++ performance parity or a fixed binary-size budget.

## 2. Current runtime and deployment

The implemented runtime is the C11 `JoyeerNativeRuntime` static archive. It
provides checked primitive operations, strings, collections, typed whole-file
input, printing, allocation, and destruction. The entry trampoline calls the
Joyeer entry point and unconditionally checks runtime allocation balance.
Allocation/free performs atomic balance accounting, and collection
clone/destroy operations can call stored callbacks indirectly. These are real
costs despite the absence of GC/ARC; see the
[native runtime details](../impl/native.md#4-runtime).

The runtime is not currently split into separately selectable `abi`, `core`,
and `std` profiles. A native program uses the target platform's C runtime and
startup/link inputs. There is no supported freestanding, embedded, kernel, or
no-libc build profile.

LLVM/LLD are dependencies of the compiler's private native-backend library,
not runtime dependencies of generated programs. The versioned `joyeer-backend`
C ABI is a compiler implementation boundary; it is not source-language C FFI.
See [building.md](../building.md) for platform prerequisites and package layout.

No checked-in comparative benchmark suite currently substantiates a 16-KB
executable limit, a sub-8-KB runtime, or a fixed per-operation overhead budget.
Such numbers must be measured for a specified target and link configuration
before becoming acceptance criteria.

## 3. Ownership operations have real costs

The source-level contract is described in [memory.md](memory.md) and
[parameter-passing.md](parameter-passing.md):

| Operation | Cost model |
|---|---|
| Copy of a trivial scalar | Copy its value; no heap ownership work. |
| Ordinary copy of a heap-backed value | Independent storage is required; recursive copying can allocate and scales with the copied data. |
| Transfer of an owned temporary | Storage may be transferred without a recursive clone. |
| Assignment over an initialized value | Acquire the replacement before destroying the previous value. |
| `borrowing` access | Does not transfer ownership; materializing a new owned value from it can still require a copy. |
| `inout` access | Exclusive access to initialized storage; updates may allocate or destroy according to the value operation. |
| `consuming` access | Transfers ownership using the required call-site `consume` marker. |
| `initializing` access | Initializes destination storage subject to the all-path initialization obligation. |

These are semantic requirements, not proof that every current lowering is
optimal. Optimization may eliminate work under the as-if rule, but it must
not make a source-level copy consume its source or change which programs are
accepted.

String concatenation and independent collection copies are especially
important in the JSON-parser workload. Repeatedly rebuilding a string can copy
its growing prefix many times. Zero outstanding allocations at program exit
establishes cleanup for that workload; it says nothing about total allocations,
peak memory, or copying throughput.

## 4. Required safety checks survive optimization

- Integer overflow traps in every supported optimization mode, including
  `-O2` and `-O3`. It is not a debug-only feature.
- Out-of-bounds indexing and invalid arithmetic retain their trapping
  semantics. An optimizer may remove a check only when doing so preserves
  those semantics.
- The current CLI supports `-O0` through `-O3`; it does not expose
  `--no-bounds-checks`, `unsafe` blocks, or runtime profiles.
- The draft `assert` / `precondition` API in spec section 9 is distinct from
  compiler-generated arithmetic and bounds checks. A future optimized-away
  `assert` must not disable required language safety checks.

The backend emits checked integer arithmetic as LLVM overflow intrinsics and
typed array accesses as explicit bounds checks plus element addressing.
Failure paths call the existing panic routine. This makes the successful
paths visible to LLVM without changing the required safety semantics or
depending on cross-library inlining of the separately compiled C runtime.
Array construction and growth validate the allocation size for the same
element layout used by indexing; append/growth and ownership operations still
use the runtime.

LLVM optimization passes can then simplify proven-safe checks and loops.
There is no implemented promise to inline every single-call-site function,
a Joyeer-specific LTO policy, or `--Wabstraction-cost` diagnostics.

## 5. Layout is not yet a public interoperability promise

The compiler determines physical layouts and access conventions through
Joyeer IR and LLVM lowering. The current native ABI is private and is not
promised to match arbitrary C structs or a future stable Joyeer library ABI.

In particular, do not infer the following from the design's value semantics:

- a string is necessarily a two-word slice at every ABI boundary;
- enum payloads already use an optimal union layout or the smallest possible
  tag;
- `Optional<Bool>` already occupies one byte through niche optimization;
- `Result<T, E>` always fits in registers or costs only one tag byte;
- generated binaries have no platform unwind metadata or C-runtime startup
  sections.

Those are representation and platform properties that need explicit design
and measurement. `Optional<&T>` is not an appropriate current layout example:
the language does not expose first-class references. Future source-language
FFI would need its own calling-convention, layout, and initialization contract.

## 6. Deferred mechanisms are not current syntax

The former cost sketches used examples involving `joyeer build --profile`,
`extern "C"`, `dyn Protocol`, user-defined generic functions, `const fn`,
`static_assert`, user-defined accessors/destructors, and layout annotations.
These are not current compiler capabilities or commands.

Possible future work includes better enum layouts, inlining/copy diagnostics,
allocator control, freestanding deployment, source-language FFI, and generics.
Each needs a language/ABI design before it can carry a performance promise.
Refer to
[the implemented language surface](../impl/supported-features.md) rather than
treating a cost sketch as an additional feature specification.

## 7. Evidence needed for a runtime budget

Before adopting numerical budgets, establish reproducible workloads and
record:

1. Target architecture, OS/SDK, compiler version, optimization level, debug
   mode, C-runtime linkage, and whether LTO or stripping was used.
2. Separate sizes for generated programs and the compiler/backend package.
   The compiler embedding LLVM is not a generated program's runtime footprint.
3. Allocation count, peak live storage, copied bytes, and cleanup balance,
   rather than using only the final outstanding-allocation count.
4. Equivalent safety and ownership semantics in comparison implementations.
5. Integer/bounds failure behavior, ordinary and consuming copies, nested
   collections, and mutation-heavy workloads at multiple optimization levels.
6. Results on supported platforms, not an extrapolation from one host.

The immediate priority is correctness of the supported subset and honest
measurements. Optimizations should preserve source locations, debug scopes,
ownership obligations, and failure semantics.

## References

- [memory.md](memory.md) - value semantics and deterministic destruction.
- [parameter-passing.md](parameter-passing.md) - the four access conventions.
- [native.md](../impl/native.md) - current LLVM/native backend and runtime.
- [supported-features.md](../impl/supported-features.md) - current executable
  surface and known limits.
- [roadmap.md](../plan/roadmap.md) - active implementation priorities.
