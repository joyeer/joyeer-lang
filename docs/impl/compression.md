# Compression

`Compression` is an optional source module backed by a separate static C
archive containing miniz 3.1.0, pinned to commit
`174573d60290f447c13a2b1b3405de2b96e27d6c`. Neither the compiler nor
`JoyeerRuntime` links miniz. Applications link it only through explicit
native-library inputs. It does not require an installed zlib, external ZIP
program, or Python.

## Building and using the module

Enable `JOYEER_BUILD_COMPRESSION=ON` when configuring the existing build.
For example, from a matching Windows ARM64 Developer shell:

```powershell
cmake --preset arm64-debug -DJOYEER_BUILD_COMPRESSION=ON
cmake --build --preset arm64-debug
& .\out\build\arm64-debug\bin\joypm.exe test `
    --compiler .\out\build\arm64-debug\bin\joyeer.exe `
    --workspace .\out\build\arm64-debug\src\library\Compression\package\Debug\workspace.toml
```

The same CMake option applies to the other supported presets. The option
defaults to `OFF`; enabling it fetches the pinned miniz source. CMake produces
a configuration-specific package containing the `Compression` module, its
native archive, a test module, and their workspace. Its generated
`project.toml` has an explicit source list, `native-bindings = true`, and a
relative `native-libraries` path. CMake, not joypm, builds that native archive.
The repository-root source-only workspace remains independent of this build.

Copy the built `Compression` directory, including `native` and `licenses`,
into a workspace, add it to the workspace members, and declare
`dependencies = ["Compression"]` in the consuming module. Use a matching
compiler/runtime, platform, and architecture. There is no automatic installed
module discovery or binary compatibility promise across toolchain revisions.

Alternatively, `cmake --install` with component `Compression` installs the
module under `library/Compression`, including the miniz MIT license and the
Joyeer license. Component `NativeDevelopment` separately installs the
runtime-owned buffer adapter headers under `include/joyeer`; applications
using only the Joyeer module do not need those C headers.

## API

```joyeer
public struct ZipEntry {
    public let name: String
    public let contents: [UInt8]
}

public enum ZipMethod { Stored, Deflate }

public func createZip(entries: [ZipEntry], method: ZipMethod,
    maximumBytes: Int): Result<[UInt8], CompressionError>

public func listZip(archive: [UInt8], maximumEntries: Int,
    maximumNameBytes: Int): Result<[String], CompressionError>

public func extractZipEntry(archive: [UInt8], index: Int,
    maximumBytes: Int): Result<[UInt8], CompressionError>
```

```joyeer
public struct ZipEntryInfo {
    public let name: String
    public let isDirectory: Bool
    public let size: Int            // uncompressed bytes
    public let compressedSize: Int
    public let crc32: UInt32
    public let isSupported: Bool    // STORE/DEFLATE, not encrypted, not a symbolic link
    // plus internal location fields
}

public func zipEntries(archive: [UInt8], maximumEntries: Int,
    maximumNameBytes: Int): Result<[ZipEntryInfo], CompressionError>

public func readZipEntry(archive: [UInt8], entry: ZipEntryInfo,
    maximumBytes: Int): Result<[UInt8], CompressionError>
```

To visit every entry, call `zipEntries` once and then `readZipEntry` for each
one. `zipEntries` parses the central directory once, with the same name and
limit rules as `listZip`, and returns entries in central-directory order.
`readZipEntry` goes straight to the entry's local header and inflates it,
so it does not parse the central directory again. In one measured run (Windows
x64, optimized) that read all 16,000 eight-byte entries of a generated archive,
the whole program took 3.9 s using `extractZipEntry(index:)` and 0.45 s using
`zipEntries` plus `readZipEntry`, of which about 0.4 s was creating the archive.
Contents are still returned whole, and the archive is still held in memory.
`readZipEntry` refuses directories and unsupported entries with
`UnsupportedArchive`, and applies the same size, CRC, limit, ZIP64 and
local-header checks as `extractZipEntry`.

An entry is only a description. Its location fields are internal and
`readZipEntry` re-validates them against the archive bytes (bounds, header
signature, method, flags, sizes, CRC), so passing an entry from another archive
returns an error such as `InvalidArchive` and never reads outside the archive.
Corrupt DEFLATE data is reported as `InvalidArchive`.
`extractZipEntry(index:)` still works, but parses the central directory on
every call.

`Stored` preserves uncompressed bytes;

`Stored` preserves uncompressed bytes; `Deflate` uses miniz compression level
6. This is ZIP, not a bare DEFLATE, zlib, or gzip stream.

Creation preserves entry order and rejects duplicate names. Names must be
nonempty UTF-8 relative file paths with `/` separators and no empty, `.`, or
`..` component, backslash, colon, or embedded NUL. Each name is at most 65535
bytes. Explicit directory creation is not part of this first API; a file can
have a nested path such as `assets/data.bin`.

Listing returns complete names in central-directory order, without the
512-byte truncation of miniz's convenience metadata field. `maximumNameBytes`
limits the sum of returned UTF-8 name bytes, excluding C terminators.
ASCII names without the ZIP UTF-8 flag are accepted. Non-ASCII names without
that flag are unsupported; CP437 and Unicode-path extra-field conversion are
not implemented. Returned names are untrusted archive metadata, not safe
filesystem paths. Listing is not a full payload/CRC validation operation.

Extraction uses the zero-based listing index, not a potentially ambiguous
name lookup. It checks CRC and size, preserves all bytes including NUL and
invalid UTF-8 content, and refuses directory and symbolic-link entries.
Extraction never writes files, creates directories, or follows archive paths.

Limits are required and inclusive. Negative limits/indices are invalid;
an absent nonnegative index is `EntryNotFound`. Archive output is capped
during native writes, and entry output is checked against both the declared
size and the actual decompression writes. Failure never returns partial
data as success. Limits bound returned data, not total memory, CPU time, or
all miniz metadata/scratch allocations. The implementation is in-memory:
Joyeer value copies and native-to-Joyeer output copies can increase peak
memory. Creation also builds separate borrowed name/content lists.

This first version supports classic single-disk ZIP with STORE and DEFLATE.
ZIP64, encryption, other compression methods, metadata preservation,
filesystem extraction, archive updates, and streaming are outside the
supported contract. Classic archive/count/entry-size limits are enforced on
creation. ZIP64 central-directory layouts are rejected when opening an
archive; ZIP64 local entry layouts are rejected when extracting that entry.
Listing alone does not inspect local entry headers.

## Errors and native ownership

`CompressionError` has `InvalidArgument`, `InvalidArchive`,
`UnsupportedArchive`, `LimitExceeded`, `EntryNotFound`, `DuplicateEntry`,
and `NativeFailure(Int)`. The last preserves `1000 + miniz error code`;
it is not a stable cross-miniz-version taxonomy. Out-of-memory while creating
Joyeer-owned output retains the runtime's existing fatal allocation policy.

The Joyeer API wraps private authorized C imports. Borrowed buffers do not
escape a call. Miniz state and allocations are local to each operation and
released on all returning paths. Successful outputs are copied with the
runtime-owned [native buffer ABI](c-interop.md#buffer-imports); miniz pointers
are never installed into a Joyeer array or disguised as integer handles.

## Coverage

Durable Joyeer fixtures exercise binary and empty entries, both methods,
archive/list/extraction boundaries, invalid names/archives, duplicates, and
actual compression of repetitive input. Both debug and optimized native
programs and the generated joypm workspace have acceptance gates.

When a Python 3.9+ interpreter is available, CMake additionally registers an
independent `zipfile` interoperability oracle covering both directions,
UTF-8/long names, CRC corruption, unsupported entries, ZIP64, and expansion
limits. Python is optional test tooling, never a source-build or runtime
dependency.
