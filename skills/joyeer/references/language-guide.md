# Joyeer language guide for the implemented subset

Use the [supported-feature boundary](supported-features.md) with this guide.
The linked examples are complete programs, not pseudocode.

## Bindings, functions, and control flow

- Prefer `let name = expression` for immutable locals and `var name = expression`
  for mutable locals. Explicit annotations use `let name: Int = expression`.
- Functions use `func add(left: Int, right: Int): Int { ... }`; call them with
  `add(left: 20, right: 22)`. Do not use Swift's `->` or Rust's `fn`.
- A function without a result annotation returns `Void`. Use `func main()` as
  the native entry point, with all executable work inside functions.
- Use braces for `if`, `else`, and `while`; parentheses around conditions are
  unnecessary. Prefer explicit `return` for clarity.
- Local mutation uses `number = number + 1`. Use a loop condition or `return`
  rather than unsupported `break`/`continue`.
- Use short-circuit `&&` to guard subsequent reads, such as checking an index
  before indexing. Use nested conditionals instead of unsupported `||`.

See [hello.joyeer](../examples/hello.joyeer) for named calls and a `while` loop.

## Scalars, strings, and bytes

`Int` is signed 64-bit, `Bool` has `true` and `false`, and a byte literal such as
`b'A'` has type `UInt8`. Use `byteToInt(value: byte)` or
`byteToString(value: byte)` for the implemented explicit byte conversions.
Do not assume arbitrary numeric conversions or C-style casts exist.

Strings support `+`, comparisons, `.count`, indexing, and `.utf8()`. Count and
index are byte-based. `.utf8()` returns an independent owned `[UInt8]`; changing
that array does not change the string. The implemented escape set includes
`\n`, `\r`, `\t`, `\"`, and `\\`; do not invent interpolation or Unicode escapes.

`print(value:)` accepts supported primitive/string values, not arbitrary
aggregates. Print fields or explicitly matched payloads instead.

## Structs, enums, and matching

Use `struct Counter { var value: Int }` and named field construction
`Counter(value: 41)`. Read a field with `counter.value`; mutable projections
require an access marker, for example `&counter.value = counter.value + 1`.
Do not introduce user-defined constructors or destructors.

A concrete payload enum can be written as
`enum Status { Empty, Value(Int), Failure(String) }`. Construct a case such as
`.Value(42)` where its enum type is known from context. `match` arms use `=>`,
not `case`/`switch`. Cases are capitalized as declared; built-in cases are also
case-sensitive. Cover every case and use `_` only for intentionally ignored
payloads or a deliberate catch-all.

## Ownership and access

| Parameter convention | Declaration fragment | Call argument | Meaning |
|---|---|---|---|
| Default or explicit borrowing | `value: String` / `value: borrowing String` | `value: text` | Read without taking ownership |
| In-place mutation | `value: inout Int` | `value: &number` | Exclusive access to initialized mutable storage |
| Ownership transfer | `value: consuming String` | `value: consume text` | Transfer ownership; do not reuse consumed storage until reinitialized |
| Initialization | `out: initializing String` | `out: &text` | Initialize storage; it must be initialized on every returning path |

Inside a function, writing through an access parameter uses `&`, such as
`&value = value + 1`. Mutable field and subscript projections also use `&`.
Ordinary local reassignment does not: `number = number + 1`.

Do not overlap an exclusive access with another argument accessing the same
storage. Compute independent values before the call where needed, without
changing evaluation semantics. Do not read consumed/uninitialized storage.

Ordinary assignment and parameter access are different: storing a heap-backed
value can clone it, whereas a default borrowing parameter does not take
ownership just because its type lacks an ampersand. Cleanup is deterministic
and compiler-managed; do not add `free`, reference counting, or custom `deinit`.

See [ownership-and-errors.joyeer](../examples/ownership-and-errors.joyeer).

## Built-in containers

- Arrays: `[Int]` (or `Array<Int>`), `[1, 2]`, and typed empty `[]`.
  Read `.count` and `values[index]`; mutate with `&values[index] = value`
  or `&values.append(element: value)`. Check bounds before indexing.
- Dictionaries: `[String: Int]` (or `Dict<String, Int>`), `["answer": 42]`,
  and typed empty `[:]`. Insert/update with `&lookup["answer"] = 42`.
  Read a known-present key with `lookup["answer"]`. Do not assume Rust-like
  `get`, `insert`, iteration, or Optional-valued lookup.
- Optional: `Optional<Int>` (or `Int?`), `.Some(value)` and `.None`.
  Inspect with `match value { .Some(number) => ..., .None => ... }`.
- Result: `Result<Int, String>`, `.Ok(value)` and `.Err(error)`.
  Match both cases; do not introduce `try`, `catch`, or propagation operators.

These are compiler-supported built-ins, not evidence for user-defined
generics. See [collections.joyeer](../examples/collections.joyeer) and
[ownership-and-errors.joyeer](../examples/ownership-and-errors.joyeer).

## File input

`readFile(path: "input.txt")` returns `Result<String, IOError>`. Match `.Ok`
to use the bytes and `.Err` to explicitly report or propagate failure.
For example, `.NotFound(code)` is an IOError payload case; do not invent
unverified error cases or a universal `.message` property.

See [read-file.joyeer](../examples/read-file.joyeer). It counts LF bytes, not
Unicode characters or an assumed universal definition of text lines.
