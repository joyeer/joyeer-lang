# JVM Decompiler Plan

> **Status:** stages 1 and 2 (control-flow graph, dominators and natural loops) are
> implemented as `moss cfg`.
> Everything after it is design, not behavior. Moss does not produce Java source
> and must not claim to until the audit gate below is met.

## Goal and honesty rule

Turn a method's bytecode into Java-shaped source an AI agent or reviewer can read.
The output is **evidence, not truth**: every method gets an explicit outcome, and
a method that cannot be recovered with confidence is reported with its exact
disassembly instead of plausible but wrong source.

| Outcome | Meaning |
|---|---|
| `decompiled` | Every stage succeeded and the strict audit passed |
| `partial` | Source emitted for recovered regions; named regions fall back to bytecode |
| `fallback` | No source; the bytecode listing is returned |
| `failed` | A structural error prevented even a listing (an existing diagnostic) |

An outcome never changes silently, and the JSON states which stages ran.

## Pipeline

```text
decoded instructions
  1. CFG                      moss cfg                          (implemented)
  2. Dominators, loops        immediate dominators, back edges, natural loops  (implemented)
  3. Stack lifting            bytecode stack -> named values per block
  4. Typed values             descriptor-driven types, locals, constants
  5. Structuring              if/else, loops, switch, try/catch/finally regions
  6. Expression recovery      fold single-use values, calls, fields, arrays
  7. Java emission            declarations, names from debug tables when present
  8. Audit                    re-check coverage and report the outcome
```

Each stage has its own module and diagnostics, and a stage never reads the output
of a later one. `moss.analysis` already holds stage 1; stages 2 and 3 extend it
before any source emission exists.

## Constraints from Joyeer

The compiler has no user-defined generics or classes, and dictionary keys are
limited to `Int`, `Bool`, `UInt8` and `String`. The design follows:

- **Concrete IR with index arenas.** Blocks, values and regions live in arrays and
  refer to each other by integer index, not by reference. The Swift engine's
  dialect-generic carriers (`TypedMIRFunction<Dialect>`) become one JVM-specific
  value model; a DEX frontend later gets its own lowering into the same arena shape.
- **Explicit sums.** Joyeer payload enums model instruction and region kinds.
- **Bounded work.** Every stage has an input-size budget (blocks, values, nesting
  depth) and fails with a diagnostic rather than running unbounded.
- **No hidden defaults.** Policies such as the maximum nesting depth are explicit
  parameters, as in the Swift `MossManaged` design.

## Order of work

1. **Dominators and loops** (done): Cooper-Harvey-Kennedy immediate dominators,
   back edges, natural loops, nesting depth, and an irreducibility report, exposed
   through `moss cfg`. Verified against an independent set-based oracle.
2. **Stack lifting** (medium): abstract-interpret the operand stack per block
   with the verifier's merge rule, producing named values and explicit phi-like
   joins at block entry. This is where `jvm.stack-mismatch` style diagnostics
   appear, since Moss does not otherwise verify stacks.
3. **Types** (medium): propagate descriptor types, resolve `Object`-typed merges
   conservatively, and treat unknown types as such rather than guessing.
4. **Structuring** (large): region recovery on reducible graphs; `try`/`catch`
   from exception ranges; irreducible or `jsr`-style code falls back.
5. **Expressions and emission** (large): fold values into expressions, then print.
6. **Audit and gates**: compare recovered control-flow with the CFG, require every
   original instruction to be accounted for, and run the whole `java.base` corpus
   through the pipeline measuring `decompiled`/`partial`/`fallback` rates before
   any release claim. A recompilation round trip is a stretch goal, not a gate.

## Reference

The Swift engine in `decompify/Packages/Moss` (`MossIR`, `MossPipeline`,
`MossManaged`, `JavaAnalyzer`) is design reference only: its stage split, strict
audit, and "attributions never suppress members" rule carry over. No code or
fixtures are translated, and licensing is not assumed to transfer.
