# Language Features for Binary Data: Remaining Work

> **Status:** proposed, nothing implemented. Goal: let library code such as a
> pure-Joyeer `crc32` or DEFLATE be written in Joyeer instead of native C (the
> current `Compression` wraps miniz, see [impl/compression](../impl/compression.md)).
> Normative rules belong in the [spec](../spec.md); adopt them there first.
> Remove items when done.

## 1. Features, in Dependency Order

| # | Feature | Today | Needed for |
|---|---|---|---|
| 1 | [Constant tables](constant-tables.md) (top-level `let`) | Design proposed; no global storage | CRC and Huffman tables, length/distance bases |

Each item needs the usual path: parser, type checking, IR and verifier,
backend, diagnostics with stable IDs, GoogleTests, and one durable native
fixture ([AGENTS.md](../../AGENTS.md)).

## 2. Decisions to Make

- **Constants (1):** immutable, initialized at compile time or once before
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
