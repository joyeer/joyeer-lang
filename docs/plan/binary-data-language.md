# Language Features for Binary Data: Remaining Work

> **Status:** proposed, nothing implemented. Goal: let library code such as a
> pure-Joyeer `crc32` or DEFLATE be written in Joyeer instead of native C (the
> current `Compression` wraps miniz, see [impl/compression](../impl/compression.md)).
> Normative rules belong in the [spec](../spec.md); adopt them there first.
> Remove items when done.

## 1. Features, in Dependency Order

| # | Feature | Today | Needed for |
|---|---|---|---|
| 1 | Bitwise operators `& \| ^ ~ << >>` | Listed in [§1](../spec/01-lexical.md) and [§5](../spec/05-expressions.md), not implemented in the frontend | CRC, bit reader/writer, LZ77 hashing |
| 2 | Explicit numeric conversions between `UInt8/16/32/64`, `Int8..64`, `Int`, `UInt` | None; `as` is reserved; no implicit conversion | Any byte-oriented code |
| 3 | Hex, binary, octal literals and `_` separators | Unsupported | Polynomials, signatures, masks |
| 4 | Constant tables (top-level `let` or equivalent) | No global storage | CRC and Huffman tables, length/distance bases |

Each item needs the usual path: parser, type checking, IR and verifier,
backend, diagnostics with stable IDs, GoogleTests, and one durable native
fixture ([AGENTS.md](../../AGENTS.md)).

## 2. Decisions to Make

- **Bitwise (1):** operand types (integers of the same type only); shift count
  type and behavior for counts `>=` width (trap, like signed overflow, or
  mask); `>>` arithmetic for signed, logical for unsigned; precedence per
  [§5](../spec/05-expressions.md). Resolve the `&` clash with the `inout`
  call-site marker ([§1.5](../spec/01-lexical.md)).
- **Conversions (2):** spelling (`as`, or named initializers such as
  `UInt32(value: x)`); which are checked (trap or `Result` on out-of-range)
  versus explicit truncating or wrapping forms. Keep `Int`/`Int64` distinct.
- **Literals (3):** range checking per contextual type, same as decimal.
- **Constants (4):** immutable, initialized at compile time or once before
  `main`; whether array constants are allowed; no mutable globals. This
  interacts with module initialization and deterministic destruction, so
  decide the model before syntax.

## 3. Acceptance

- Per feature: accepted and rejected programs, boundary values, and
  Debug and Release native runs ([building](../building.md)).
- Milestone: a pure-Joyeer `crc32` in `Foundation` matching the miniz/zlib
  result on known vectors. DEFLATE is a later, separate effort that also needs
  the byte-copy and performance work noted in [library](library.md).
- Update [supported-features](../impl/supported-features.md) as each lands.
