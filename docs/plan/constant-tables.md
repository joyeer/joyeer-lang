# Constant Tables: Design Proposal

> **Status:** proposal for review, nothing implemented. Part of
> [binary-data-language](binary-data-language.md). Goal: immutable top-level
> values such as a CRC or Huffman lookup table, built once at compile time
> instead of on every call. Adopt the rules in the spec before implementing.

## 1. Today

A top-level `let` is parsed and name-resolved, then rejected during IR lowering
("top-level bindings are not supported by primitive IR lowering", see
[irlowering.cpp](../../src/compiler/ir/irlowering.cpp)). There is no global
storage and no initialization order.

## 2. Recommended Design: Compile-Time Constants

```joyeer
public let crcPolynomial: UInt32 = 0xEDB8_8320
let crcTable: [UInt32] = [0, 0x7707_3096, 0xEE0E_612C]   // 256 entries in practice

func update(crc: UInt32, byte: UInt8): UInt32 {
    return crcTable[(crc ^ (byte as UInt32)) &as Int & 0xFF] ^ (crc >> 8)
}
```

Rules:

- **Only `let`.** A top-level `var` stays rejected.
- **Constant initializer.** Allowed: literals, array literals, prefix `-`/`~`/`!`,
  binary arithmetic, bitwise, comparison and logical operators, `as`/`&as`, and
  references to other constants. Function calls, `if`/`match` and anything that
  needs a run-time value are rejected with a stable diagnostic.
- **Evaluated by the compiler.** Overflow, division by zero, an out-of-range
  `as`, or a shift count out of range is a compile-time error, not a trap.
  Cycles between constants are diagnosed; declaration order does not matter.
- **Types allowed (first version):** `Bool`, all integer types, `Float`,
  `Double`, `String`, and arrays of those, including nested arrays. Structs,
  enums, dictionaries and optionals are deferred.
- **Visibility:** the usual `public`/`internal`/`private`. A `public` constant
  needs an explicit type, like a public function signature.
- **Use is a borrow.** Reading `crcTable[i]`, `crcTable.count`, or passing it to
  a `borrowing` parameter uses the static data directly, with no copy.
  Binding it to a `var`, or any use that needs an owned value, makes an
  ordinary independent copy. `inout`, `consuming` and assignment to a constant
  are errors.

## 3. Implementation Sketch

| Stage | Change |
|---|---|
| Name resolution / type checking | Allow top-level `let`; require a constant initializer; detect cycles and unsupported types |
| New constant evaluator | Width-aware evaluation of the allowed operators on the typed AST, producing integer, float, bool, string, or array values |
| Joyeer IR | A constant-reference value (borrowed, never destroyed) plus module-level constant data; verifier checks type and category |
| Backend | Scalars fold to immediates. Strings and arrays become private read-only data, with arrays as `{data, count, count}` descriptors; no allocation, no tracked balance |
| Runtime | No change expected. Cloning an array must not depend on capacity or ownership; verify this before relying on it |

The evaluator is the largest part. It also gives a natural home for compile-time
range checks that `x as T` already performs on literals.

## 4. Alternatives Considered

- **Run-once initializers before `main`.** Allows any expression, but needs a
  defined initialization order across modules, failure handling, and exit-time
  destruction. Much larger and interacts with ownership; defer until there is a
  need that constants cannot meet.
- **Table built in a function at each call.** What is possible today. Correct,
  but pays construction cost every call.

## 5. Open Questions

1. Is compile-time-only acceptable for the first version?
2. Is excluding structs, enums, and dictionaries acceptable for now?
3. Should a `public` constant be usable from other modules by short name and
   qualified name, like functions? (Proposed: yes.)
4. Should array constants be limited in size (for example 1 MiB of data) to
   keep compile time and object size bounded?

## 6. Acceptance

- GoogleTests: accepted and rejected initializers, cycles, overflow at compile
  time, type restrictions, and misuse (`inout`, assignment, `var`).
- Native fixtures at `-O0` through `-O3`: scalar, string and array constants,
  a 256-entry table, borrow versus copy, nested arrays, and cross-module use.
- A pure-Joyeer `crc32` in `Foundation` using a constant table, checked
  against known vectors.
- Update [supported-features](../impl/supported-features.md) and the spec.
