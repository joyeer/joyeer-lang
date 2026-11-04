# Moss Delivery Plan

## 1. Goal and current boundary

Build an AI-oriented reverse-engineering CLI in Joyeer. Start with lossless
JVM bytecode disassembly, then add archive support, other instruction
backends, and progressively deeper analysis.

The current project implements the M1 binary foundation, M2-M4 JVM
inspection/disassembly queries (per method and whole class), classic ZIP/JAR
entry listing and selection, and a structural control-flow graph (`cfg`). Native
and process regressions pass on Windows ARM64 and x64. M5 is partially verified:
the `javap` differential passed on self-authored fixtures and on sampled
javac-compiled JDK 25 `java.base` classes (see the JVM contract's validation
section). Other native platforms, licensing, and release packaging remain open.
No decompiler exists yet. See [binary](binary.md) and [JVM/CLI contracts](jvm.md).

This plan intentionally does not start a full Swift-to-Joyeer translation
or require Joyeer to implement the entire Swift language. Implementation
uses the [current Joyeer subset](../../../docs/impl/supported-features.md).

## 2. First product: JVM disassembler v0

### Included

- One ordinary local `.class` file per invocation, with bounded acquisition.
- Class identity, version, access flags, constant-pool metadata, fields,
  methods, descriptors, and exact Code attribute locations.
- All documented JVM instruction encodings for the declared supported
  classfile versions, including `wide`, signed immediates, branches,
  `tableswitch`, and `lookupswitch`.
- Typed operands, constant-pool references, raw instruction bytes, encoded
  lengths, file offsets, and method-relative offsets.
- Exception-table regions; bounded optional line/local-variable metadata.
- Deterministic plain-text output in `javap -c -p -l` layout with explicit errors.
- Method selection, offset/range queries, and bounded paginated results.

JVMS SE 25 is the format reference. The explicit structural version policy
is major 52..69/minor 0, plus Java 25 preview only with opt-in. Self-authored
fixtures cover Java 8, 17, 21, 25 and all intermediate version numbers.
Unknown versions and legacy modern-version opcodes fail explicitly; a
familiar magic number does not prove support. Real javac/JDK corpus
validation is still pending.

### Excluded

- APK/DEX, ELF, Mach-O, PE, and raw machine-code inputs. (Classic ZIP/JAR
  entry listing and selection were added after v0; see the JVM contract.)
- Java source reconstruction, type inference, SSA, or a complete JVM
  stack/type verifier.
- Executing analyzed classes, fetching dependencies, or resolving an
  external classpath.
- GUI, MCP server, network service, model invocation, or automatic patching
  of analyzed binaries.
- Cross-platform or performance claims without recorded native evidence.

Disassembly is structural decoding, not decompilation. Parsing and static
reference/boundary checks do not constitute full JVM verification.

## 3. Architecture and ownership

```text
CLI query
  -> bounded input acquisition
  -> classfile structural reader
  -> immutable class/method/constant inventory
  -> JVM instruction decoder
  -> reference and boundary checks
  -> javap-layout text output
```

Keep parsing, decoding, semantic analysis, and presentation separate.
Do not construct a rich decompiler object graph merely to print bytecode.

### Implemented modules

| Module | Responsibility | Dependencies |
|---|---|---|
| `moss.binary` | Input ownership, checked cursor reads, spans, byte/numeric helpers | None |
| `moss.model` | Artifact IDs, instruction facts, typed operands, diagnostics | None |
| `moss.jvm` | Constant pool, classfile/Code reader, JVM decoder, checked references | `moss.binary`, `moss.model` |
| `moss.output` | Escaping, serialization, text formatting, bounded output | `moss.model` |
| `moss.cli` | Queries, command validation, selection, budgets | `moss.jvm`, `moss.output` |
| `moss.app` | Host I/O and final CLI status handling | `moss.binary`, `moss.cli`, `moss.model` |

The [workspace](../workspace.toml) explicitly registers nine member directories,
each with its own single-module manifest. Binary/JVM/CLI tests are separate
test modules. `moss.cli` also directly uses `moss.binary` and
`moss.model`. Introduce further modules only with real sources and explicit
test targets; do not create empty placeholders.

The output module must not import a JVM decoder. A JVM adapter projects
format-owned facts into output records. Shared algorithms must not depend
on CLI options or an application workspace.

### Data and storage rules

- Own each input once where practical; retain source spans as offset/length
  records, not copies of every attribute or method body.
- Keep the cursor limited to input coordinates and a current position.
  Do not place accumulating class inventories inside a cursor passed for
  every read.
- Bounds checks use remaining length before addition or indexing.
  No malformed input may trigger an arithmetic or subscript trap.
- Represent absent Code on native/abstract methods explicitly, rather than
  deleting those methods from the inventory.
- Use enums for operand kinds and fallible operations. No empty-string
  replacement for a failed constant lookup or string decode.
- Represent graphs using owned arrays and IDs when later phases need them.
  Shared ownership, classes, or ARC are not prerequisites.
- Keep tool analysis models independent of the Joyeer compiler's internal IR.

## 4. Dependency-ordered milestones

| Milestone | Prerequisites | Deliverable | Acceptance gate |
|---|---|---|---|
| M0: Managed skeleton | Existing fern/compiler | Manifest, help/version entry, smoke target, this plan | fern check/build/run/test in Debug and Release; unsupported commands fail |
| M1: Binary foundation | M0 | Checked big-endian reads, signed conversion, spans, domain errors, fixed input/output policy | Truncation at every read width; numeric extrema; invalid ranges; no traps; native tests at `-O0` and `-O2` |
| M2: Classfile reader | M1 | Constant pool, class/field/method inventory, bounded attributes and Code spans | Generated class fixtures; all supported constant tags; dual-slot constants; exact offsets; absent Code; malformed references and lengths |
| M3: JVM decoder | M1, M2 | Complete supported opcode decoding and typed operands | Opcode/operand matrix; variable-length boundaries; branch/exception checks; differential comparison with javap |
| M4: AI query/output | M2, M3 | inspect/methods/disasm queries, javap-layout text, selection and pagination | Byte-exact comparison with javap; deterministic ordering; escaped text; explicit partial/failure policy; budget boundaries |
| M5: v0 hardening | M1-M4 | Version matrix, malformed corpus, cost measurements, usage docs | All declared native gates; fixture provenance; allocation balance; reproducible measurements; no unsupported capability claims |

M1-M4 are implemented within the native Windows ARM64 boundary below.
M5 remaining work is the next acceptance task: use an explicit installed JDK
for differential and real-compiler fixtures, then verify broader platforms
and release boundaries. Do not label these external gates complete.

M0 was verified on 2026-10-10 with the existing Windows ARM64 toolchain:
fern check passed, Debug/Release build and run passed, and the single
declared native test target passed ten checks in each configuration.
Unsupported analysis invocations returned 2 with stderr and empty stdout.
These bootstrap results are not decoder acceptance.

M1 was verified on 2026-10-10 with the same native development toolchain.
Full fern Debug/Release suites passed 673 binary checks and the unchanged
ten CLI checks per configuration, including exact input-budget boundaries.
Separate locked-file, final-junction, and Windows NUL-device probes passed.
Successful numeric-read LLVM was inspected for borrowed input and scalar
cursor storage. [Binary contracts and evidence](binary.md) retain the
non-confinement, input-identity, and non-benchmark limitations.
M2-M4 native Debug/Release suites pass 1475 JVM checks plus 673 binary and
ten CLI checks per configuration. Process integration passes 678 checks per
executable, including 217 opcode/encoding cases and exact 8 MiB output.
Stable queries, typed facts, metadata budgets, method-index selection, and
exclusive output are implemented. [JVM contracts](jvm.md) record partial
attribute/type-verification boundaries, measurements, and unexecuted
JDK/platform gates. M5 is not fully accepted.

### M1: Binary foundation

Start with current supported features: `Int`, `UInt8`, `String` byte
indexing, concrete structs/enums, `Result`, and explicit borrowing/inout.

- Implement u1/u2/u4 reads using multiplication/addition in signed 64-bit
  `Int`. These unsigned values fit; conversion does not need bitwise syntax.
- Convert signed 8/16/32-bit operands explicitly after an unsigned read.
- Keep 64-bit unsigned bit patterns as high/low components or raw bytes
  until Joyeer has a tested suitable numeric type/conversion contract.
- Errors include a stable code, file byte offset, and contextual details.
- Choose ownership-transfer and cursor APIs that do not clone the entire
  input per read. Inspect emitted IR and measure rather than assuming COW.
- Add a native test executable for reader and cursor boundaries.

### M2: Classfile reader

- Validate magic, version policy, all counts, and the final file position.
- Use constant-pool index zero as invalid and model the unusable second
  slot of long/double constants explicitly.
- Check constant index bounds and expected tag before dereferencing.
- Respect each attribute's declared length with a bounded subregion.
  Unknown attributes are retained as raw spans or skipped exactly, never
  interpreted using an unrelated parser.
- Validate Code length, max stack/locals, exception metadata, and nested
  attributes without reading outside their enclosing attribute.
- Preserve Float/Double raw bits, including NaN payloads and signed zero;
  numeric pretty-printing is not a prerequisite.
- Handle Modified UTF-8 separately from UTF-8. Preserve raw bytes and UTF-16
  code units where necessary, including unpaired surrogates in string
  constants; use explicit escapes instead of lossy replacement.

### M3: JVM decoder

- Preserve raw opcode, effective opcode, `wide` encoding, offset, and size.
  Do not narrow a wide signed iinc increment back to 8 bits when rendering.
- Interpret branch displacements relative to the instruction opcode, not
  the next instruction or file origin.
- Align switch payloads relative to the start of method bytecode, checking
  all four opcode alignment positions. Padding bytes are consumed without
  inventing a zero-value constraint.
- Bound switch counts by remaining bytes before loops/allocations; validate
  low/high ordering and strictly increasing lookup keys.
- Validate branch destinations against decoded instruction starts, never
  allowing a target inside wide operands or switch tables.
- Validate exception-table start/end/handler rules, including exclusive end.
- Reject invalid opcodes/operand shapes with offsets. Version-specific
  exclusions remain explicit rather than silently treating bytes as data.
- Share opcode metadata between decoder and renderer where possible.

### M4: AI-facing queries and output

Implemented commands:

```text
moss inspect sample.class
moss methods sample.class
moss disasm sample.class --method compute --descriptor "(I)I"
moss disasm sample.class --method compute --descriptor "(I)I" --offset 0 --limit 100
moss disasm sample.class --method-index 0 --output result.txt
```

Use class internal name + method name + descriptor as the method identity.
Bare method-name matches that select overloads are errors, not guesses.

The output is plain text in the layout of `javap -c -p -l`, documented in
[JVM output](jvm.md). An earlier schema-versioned JSON/NDJSON format was removed:
the consumers of the tool are people and agents that already read javap, and a
second machine format added cost without a user.

Output requirements:

- Stdout contains only the chosen output; `--output` writes a new file.
- Diagnostics/progress go to stderr. Distinguish malformed, unsupported,
  usage, host-I/O, and output failures with stable diagnostic codes.
- Pagination succeeds with an explicit `// more: continue with --offset N` line. It
  is not the same as an error after partial output, which exits nonzero.
- Define offset selection precisely: instruction-start offsets only;
  offsets inside operands reject. Define terminal end-of-method behavior.
- Print non-ASCII text and isolated surrogates as `\uXXXX` so names and strings stay
  exact; print float and double constants as Java's `toString` does.
- Keep future JVM method offsets, DEX byte offsets, and native virtual
  addresses explicitly typed/namespaced; do not equate them.

### Initial v0 resource policy

The following byte/record limits are implemented and tested. They are not
total heap usage or elapsed-time guarantees:

| Resource | Initial policy | Required boundary tests |
|---|---|---|
| Input class file | 16 MiB inclusive, with one extra sentinel byte | Exact ceiling, ceiling + 1, truncation, missing/nonregular input |
| Method Code length | 1..65535 bytes, as required by JVMS | Zero, maximum, larger length, enclosing attribute mismatch |
| Instruction page | Default 1000 records; maximum requested limit 10000 | 0/negative/maximum/above maximum; exact next offset |
| Serialized output | 8 MiB per invocation/page | Exact ceiling, above ceiling, oversized individual switch/constant |
| Utf8 payloads | 1 MiB encoded bytes total | Over-limit class rejects explicitly |
| Metadata records | 262144 charged entries total | Counts and aggregate limits reject before unbounded growth |

Do not allocate from an untrusted count before validating its byte budget.
Stop with an explicit budget error if one record cannot fit; never discard
switch entries or shorten constants invisibly. No elapsed-time or memory
performance guarantee is advertised before M5 measurements.

## 5. Validation strategy

Every implementation milestone adds explicit Joyeer native test modules
with their own manifests and workspace membership. The package manager
does not discover test functions/files.
Test executables return nonzero on failures.

- Reader tests: byte-exact inputs, NUL, signed extrema, u4 maximum, cursor
  positions, and all truncation boundaries.
- Parser tests: generated minimal fixtures and boundary cases for every
  supported constant/attribute shape, corrupt indexes/tags, and lengths.
- Decoder tests: every opcode family, maximum local indexes, wide iinc
  extrema, backward/forward branches, switches, and invalid targets.
- Differential tests: compare structured facts with `javap -v -c -p -s`
  from an explicitly chosen JDK. Do not rely on formatted text equality
  alone or on a decompiler as the only oracle.
- CLI tests: actual process exit/streams, argument handling, exact schema,
  deterministic output, budgets, pagination, and malformed input.
- Run Debug and Release tests on each claimed native platform/architecture.
  An ARM64 run does not establish x64 or POSIX support.
- Record input size, decoded count, elapsed time, peak memory, allocation
  behavior, compiler revision, and native target for performance baselines.

JDK tools are development/test prerequisites, not dependencies of a released
Moss executable. Tests must use explicit tool paths and argument arrays.
Prefer self-authored javac fixtures; inventory provenance and permissions
before copying existing Swift-package fixtures or third-party corpora.

The earlier research probe demonstrated a real class header and a small
raw-bytecode subset in 24 cases at both optimization levels. It is not
part of this package and does not satisfy M1-M5 acceptance.

## 6. Joyeer evolution, driven by measured needs

Language/runtime changes belong in the Joyeer repository with their own
specification, frontend, IR/backend, runtime, and regression gates. Moss
must not depend on undocumented compiler special cases.

| Priority | Capability | Trigger and current alternative |
|---|---|---|
| Early | Checked Int-to-UInt8 conversion | Avoid byte-encoding lookup tables; reading can start with byteToInt |
| Early | Capacity-aware byte/text builders and batch append | Large formatting/encoding; initially use bounded records/chunks |
| Before reliable pipe-oriented v0 output | Fallible stdout writing | Handle broken pipes/write/flush errors; current print cannot prove output success |
| After initial correctness | Scoped read-only byte views/slices | Measured duplicated input/attribute storage; begin with owner + spans |
| DEX/native stage | Fixed-width integers, bitwise/shift and conversion contracts | Masks, LEB128, unsigned addresses; raw components suffice for JVM v0 facts |
| Large indexing/analysis stage | Hashed collections, richer collection operations, user generics | Cross-class xref and shared CFG/SSA algorithms; use explicit arrays/IDs first |
| Archive/native stage | Random-access I/O and controlled C ABI/link inputs | ZIP/DEFLATE or Capstone; no current source-level FFI assumption |

Fallible stdout remains a reliability gate for normal pipe-oriented output,
not a reason to postpone the reader/decoder. The implemented alternative
uses exclusive `--output` file writes; a successful exit with stdout output does
not prove complete delivery.

Classes, ARC, closures, async, dynamic plugins, and a package registry are
not prerequisites for the first synchronous CLI.

## 7. Later product phases

| Phase | Prerequisites | Scope and exit gate |
|---|---|---|
| JAR | Accepted `.class` v0 and compression/I/O contract | In-memory entry selection, ZIP/DEFLATE, CRC, duplicate names, multi-release policy, decompression budgets; no mandatory disk extraction |
| DEX | Stable model/output boundary and required numeric helpers | Checked tables/LEB128, byte-offset decoding, register/literal facts; do not reuse JVM stack semantics blindly |
| Native | Format readers and formal native-library interop/linking | One explicit architecture first, e.g. ARM64 with Capstone; distinguish mapped addresses from file offsets |
| Analysis | Accepted decoders and efficient indexed storage | References, CFG and exceptions, then SSA/data flow with explicit format policies |
| Decompilation | Verified analysis and managed type model | Typed MIR/HIR, structured recovery, source emission, recompilation and original-member audit |

Before native multi-format auto-detection, handle ambiguous magic numbers:
Java classfiles and Mach-O FAT can both begin with CAFEBABE. Use structural
probes and an explicit format override, not magic-only success.

Shared instruction/query contracts should grow from at least two real
backends. Do not build a universal semantic IR or dynamic plugin framework
before the second backend demonstrates what must be shared.

## 8. Reference material and migration boundaries

Study and adapt contracts/algorithms, not application actors or handlers:

- [JVMS classfile structures](https://docs.oracle.com/javase/specs/jvms/se25/html/jvms-4.html)
- [JVMS instruction encodings](https://docs.oracle.com/javase/specs/jvms/se25/html/jvms-6.html#jvms-6.5)
- [Joyeer host operations](../../../docs/spec/18-host.md)
- [fern workflow and limitations](../../../docs/impl/fern.md)

Do not mechanically translate force unwraps, unchecked indexes, ignored
errors, or object ownership into Joyeer. Preserve original bytes and report
format failures explicitly.

Licensing, third-party notices, native dependency inventories, packaging,
and supported-platform claims must be settled before any release. A new
project name or package version does not grant redistribution permissions.
