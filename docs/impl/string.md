# String Implementation

> **Status:** The native v0.1 string representation, ownership operations,
> comparisons, byte indexing, owned `utf8()` byte arrays, concatenation,
> printing, and file-input path are implemented. Broader formatting remains
> future work.

## Representation

Joyeer `String` is a byte-preserving value represented in LLVM and the C ABI as:

```c
typedef struct JoyeerString {
    const uint8_t* data;
    int64_t count;
} JoyeerString;
```

Strings are not required to be NUL terminated. The count is authoritative, so
embedded NUL bytes from `readFile(path:)` are preserved. The native runtime
implementation is in `include/joyeer/native/runtime.h` and
`lib/native/runtime.c`.

## Ownership

Heap-backed strings have value semantics at the Joyeer level:

- clone allocates independent storage and copies exactly `count` bytes;
- destroy releases owned storage and clears the handle;
- compiler-generated Joyeer IR `copy`, `take`, and `destroy` operations route
  through per-type LLVM helpers;
- normal scope exit, early return, aggregate destruction, collection growth,
  and tagged payload cleanup all preserve ownership;
- the native entry point requires the runtime allocation count to return to
  zero.

Borrowed strings are cloned before entering owning storage. Owned temporaries
are moved without an extra clone where lowering can prove the transfer.

## Operations

The current surface includes:

- string literals;
- `String.count`;
- checked byte subscript returning `UInt8`;
- concatenation, equality, and ordering;
- `print(value:)`;
- explicit `byteToInt(value:)` and `byteToString(value:)` conversions;
- `String.utf8() -> [UInt8]`, producing an independent owned byte array;
- `readFile(path:) -> Result<String, IOError>` with stable categories and the
  original platform code;
- recursive storage inside structs, enums, arrays, and dictionaries.

Byte indexing is deliberate: Joyeer does not currently claim Unicode scalar or
grapheme indexing. Display-column handling in diagnostics is also byte based.

## ABI

Runtime calls decompose string handles into pointer/count fields or use output
pointers for owned aggregate results. C structs are not passed by value across
the generated-module/runtime boundary, avoiding target-specific aggregate ABI
drift.

Path input rejects embedded NUL bytes. Windows currently uses the active
narrow-character CRT path encoding; a future Unicode path API belongs to the
standard library rather than the core string layout.

## Remaining work

- an explicit scoped zero-copy/read-only byte-view design;
- Unicode scalar/grapheme APIs;
- formatting and aggregate printing;
- small-string or other layout optimization after the value-copy model is
  finalized;
- streaming and incremental text/file APIs.

## Validation

```pwsh
ctest --test-dir build -L native-runtime --output-on-failure
ctest --test-dir build -L native --output-on-failure
ctest --test-dir build -L file-io --output-on-failure
ctest --test-dir build -L json-parser --output-on-failure
```

Tests cover independent clones, destruction, embedded bytes, concatenation,
comparison, checked indexing, nested aggregate/collection ownership, file I/O,
JSON parsing, and final allocation balance.