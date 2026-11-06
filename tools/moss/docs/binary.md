# Binary Foundation

**Status:** M1 is implemented in `moss.binary`. The reader and bounded input
acquisition are accepted on native Windows ARM64 with the current development
compiler at both `-O0 -gfull` and `-O2 -g0`. This is not a classfile parser,
bytecode decoder, general I/O library, or cross-platform acceptance claim.

The sources are [reader.joyeer](../src/binary/reader.joyeer) and
[input.joyeer](../src/binary/input.joyeer), explicitly registered in
[module manifest](../src/binary/project.toml). The root
[workspace](../workspace.toml) includes this member. Consumers write
`import moss.binary` and use public short names such as `Cursor` and
`readUnsigned`. Full module qualification remains available for name
collisions.

## Ownership and coordinates

`String` is used as a byte-preserving input because the current Joyeer host
functions return owned strings that can contain arbitrary bytes. No UTF-8
validation or text decoding is applied to the contents.

- `Span` has public immutable `offset` and `length` fields.
- `Cursor` has public immutable `start` and `end` fields and a public mutable
  `position`.
- All coordinates are absolute byte offsets in the supplied input.
- A region is `[offset, offset + length)`. A cursor operates within
  `[start, end)`, with `start <= position <= end`.
- An empty span or cursor at EOF is valid. Positive-width reads at its end
  return a truncation error.

The cursor owns only three integers, not a string, array, or accumulated
parser model. Every operation borrows `input`; mutable operations receive
`cursor: inout Cursor`. Successful primitive reads do not clone the input.

Coordinates are not owner identities. Callers must keep a cursor/span with
its original input; another equal-sized or larger input is not distinguishable
by bounds validation. These records are not escaping borrowed views, checked
arena handles, or a confinement mechanism.

## API

The types and signatures below are in `moss.binary`:

```joyeer
func span(input: borrowing String, offset: Int, length: Int, context: borrowing String): Result<Span, BinaryError>
func fullCursor(input: borrowing String): Cursor
func boundedCursor(input: borrowing String, region: borrowing Span, context: borrowing String): Result<Cursor, BinaryError>
func remaining(input: borrowing String, cursor: borrowing Cursor, context: borrowing String): Result<Int, BinaryError>
func takeSpan(input: borrowing String, cursor: inout Cursor, length: Int, context: borrowing String): Result<Span, BinaryError>
func skip(input: borrowing String, cursor: inout Cursor, count: Int, context: borrowing String): Result<Void, BinaryError>
func seek(input: borrowing String, cursor: inout Cursor, offset: Int, context: borrowing String): Result<Void, BinaryError>
func readUnsigned(input: borrowing String, cursor: inout Cursor, width: Int, context: borrowing String): Result<Int, BinaryError>
func readSigned(input: borrowing String, cursor: inout Cursor, width: Int, context: borrowing String): Result<Int, BinaryError>
func readBits64(input: borrowing String, cursor: inout Cursor, context: borrowing String): Result<Bits64, BinaryError>
```

`span` validates its coordinates against the whole input. `boundedCursor`
revalidates the supplied span and starts at its offset. To derive a child
region within a parent cursor, use `takeSpan` on the parent first, then create
a bounded cursor from the returned span. Merely constructing a span does
not establish containment within some other cursor.

`takeSpan` advances by the validated length without copying any bytes.
`skip` has the same bounds and advancement behavior but returns unit.
`seek` accepts an absolute offset from the cursor start through its exclusive
end, inclusive; it cannot escape that region.

All public cursor state is checked before arithmetic or subscripting.
Negative/extreme lengths and forged cursor fields return typed errors.
Failed operations leave `position` unchanged, including a truncated
eight-byte read. This is per-operation atomicity, not rollback of a sequence
of already successful parser operations.

### Numeric reads

Reads are big-endian. `readUnsigned` and `readSigned` accept only widths
1, 2, and 4:

| Width | Unsigned result | Signed result |
|---|---|---|
| 1 | 0..255 | -128..127 |
| 2 | 0..65535 | -32768..32767 |
| 4 | 0..4294967295 | -2147483648..2147483647 |

The supported values fit in Joyeer's signed 64-bit `Int`. Unsigned reads
use multiply/add; signed reads explicitly convert the encoded two's-complement
value after a successful unsigned read. Unsupported widths are rejected
before cursor validation or advancement.

`readBits64` returns `Bits64` with public `high` and `low` fields, each an
unsigned 32-bit value held in `Int`. It checks the full eight-byte budget
before reading either half. It does not interpret a signed integer or
Float/Double and does not combine an unsigned 64-bit value into `Int`.
NaN payloads, signed-zero patterns, and all-one bit patterns remain lossless.

For example, this helper reads a four-byte value without owning the input:

```joyeer
import moss.binary

func readMagic(input: borrowing String): Result<Int, BinaryError> {
    var cursor = fullCursor(input: input)
    return readUnsigned(input: input, cursor: &cursor,
        width: 4, context: "class.magic")
}
```

## Reader errors

`BinaryError` retains public `kind`, `offset`, `requested`, `available`,
and `context` fields. `binaryErrorCode(error:)` returns a stable code:

| Kind | Code | Meaning |
|---|---|---|
| `InvalidRange` | `binary.invalid-range` | Invalid span, negative length/count, or seek outside the region |
| `InvalidCursor` | `binary.invalid-cursor` | Malformed cursor or a region outside the backing input |
| `InvalidWidth` | `binary.invalid-width` | Numeric width is not 1, 2, or 4 |
| `Truncated` | `binary.truncated` | A valid cursor has fewer bytes than requested |

For truncation, `offset` is the current cursor position, `requested` is the
entire operation's byte count, and `available` is the remaining region length.
Invalid cursor/width diagnostics use zero available bytes rather than deriving
unsafe counts from invalid state. Invalid seek reports the attempted offset.
The caller-supplied context is preserved exactly, for example `Code.max_stack`.

These are recoverable library results. Final CLI boundaries must report them
and return a nonzero status, not replace them with a value or empty string.

## Bounded file acquisition

```joyeer
func maximumInputBytes(): Int
func readInput(path: String): Result<String, InputError>
func inputErrorCode(error: InputError): String
func inputHostCode(error: InputError): Int?
func fileSystemCode(error: FileSystemError): Int
```

The fixed inclusive ceiling is **16777216 bytes (16 MiB)**. Acquisition first
classifies the final entry with `fileKind`, accepting only `.File`. It then
calls `readFilePrefix` with 16777217 bytes, rejecting the extra sentinel byte.
It never acquires the rest of an oversized ordinary file.

Empty regular files succeed at this layer; a future format parser decides
whether their contents are valid. There is no extension or classfile-magic
check in this module. Returned bytes are independently owned.

| Case | Code | Payload |
|---|---|---|
| `InspectFailed` | `input.inspect-failed` | Original path and typed filesystem error |
| `NotRegular` | `input.not-regular` | Original path and observed FileKind |
| `ReadFailed` | `input.read-failed` | Original path and typed filesystem error |
| `TooLarge` | `input.too-large` | Original path and the inclusive ceiling |

`inputHostCode` returns `.Some(code)` for inspect/read errors and `.None`
for classification/size rejections. It retains the native host code; it does
not invent a host failure for a domain error.

Host path/argument rules follow the
[Joyeer filesystem contract](../../../docs/spec/18-host.md):
strict UTF-8, no embedded NUL, wide Windows APIs, and ordinary host-relative
resolution. Paths are not canonicalized or confined.

Classification and reading are separate operations. Ancestor links and
replacement races remain host behavior; this is not a race-free regular-file
open, filesystem sandbox, or timeout contract. The byte ceiling is an
acquisition limit, not a bound on total heap usage or all copies elsewhere
in an application. A TooLarge result does not establish the full file size.

## Tests and scoped evidence

[The binary test target](../tests/binary/main.joyeer) uses a self-authored
[256-byte octet fixture](../tests/fixtures/README.md), checked byte by byte.
It covers:

- All byte values, endian ordering, unsigned maxima, and signed extrema.
- Every truncation boundary for 1/2/4/8-byte operations.
- Invalid widths, forged cursors, minimum/maximum Int coordinates, bounded
  subregions, seeks, skips, empty spans, and atomic failures.
- Lossless high/low 64-bit patterns and nonzero absolute offsets.
- Arbitrary-byte and empty-file acquisition, missing/invalid paths,
  nonregular inputs, exact 16 MiB acceptance, and ceiling-plus-one rejection.
- Stable diagnostic codes, offsets, requested/available counts, context,
  original host paths, and native error payloads.

On 2026-10-10, the full fern native suites passed on Windows ARM64 in both
Debug and Release: **673 binary checks and 10 CLI checks per configuration**.
The profiles use `-O0 -gfull` and `-O2 -g0` respectively. Separate actual host
probes passed for a locked ordinary file (`input.read-failed`, native code
32), a final directory junction, and the Windows NUL device; the latter two
returned `input.not-regular` in both configurations.

The test executable also accepts this explicit host-probe mode:

```text
moss-binary-tests --expect-input-error <path> <stable-code>
```

Generated acquisition fixtures use exclusive file creation directly under
the existing ignored `target` directory. Only successfully claimed files
are removed; no temporary directory is created or recursively cleaned.
Debug and Release suites also passed while running concurrently.

Targeted emitted-LLVM inspection confirmed the successful numeric-read loop
uses the borrowed input pointer/length and scalar cursor storage, with no
input clone or heap allocation in that loop. Error contexts can allocate.
This is not a complete allocation or peak-memory benchmark.

No native POSIX or x64 acceptance, FIFO or complete permissions matrix,
classfile parsing, or bytecode-decoding coverage is claimed by M1.
