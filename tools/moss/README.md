# Moss

Moss is an AI-oriented reverse-engineering CLI written in Joyeer and built
by fern. It inspects JVM `.class` files (standalone or inside a JAR/ZIP),
lists archive entries and methods, and disassembles selected method bytecode
without reconstructing Java source.

**Status: working early-development JVM CLI, not a published release.**
The parser accepts Java 8..25 classfile versions (major 52..69, minor 0);
Java 25 preview requires explicit opt-in. Structural and native regression
tests pass on Windows ARM64. JDK/javap differential acceptance, other native
platforms, licensing, and release packaging remain open.

See the [CLI and schema contract](docs/jvm.md), the
[binary foundation](docs/binary.md), and the [remaining delivery plan](docs/plan.md).

## Capabilities

- Bounded 16 MiB input acquisition, checked big-endian readers, and stable
  diagnostics with file-byte offsets.
- Constant pools, fields, method descriptors, raw attribute spans, Code,
  exception regions, line/local-variable tables, and module metadata.
- All 198 permitted modern base opcodes, 11 modern wide forms, and both
  switch encodings; reserved and legacy `jsr`/`ret` forms reject.
- Raw bytes, instruction sizes, file and method-relative offsets, typed
  operands, checked constant references, and branch/debug/exception boundaries.
- Overload-safe name/descriptor or inventory-index method selection.
- Plain-text output in the layout of `javap -c -p -l` (JDK 25): class and member
  declarations with generics, constant-pool comments, `Exception table`,
  `LineNumberTable`, and `LocalVariableTable`. Bounded pages and an 8 MiB output budget.
- Explicit ordinary-file output that never overwrites an existing destination.
- Control-flow graphs (`moss cfg`): basic blocks, labelled edges including exception
  edges, reachability, immediate dominators, natural loops, irreducibility. Structural only; the foundation for later decompilation.
- Whole-class disassembly (`disasm` without a method selector), `--class` lookup by
  class name, and `entries --prefix`.
- Classic ZIP/JAR input (STORE and DEFLATE): `moss entries app.jar` lists names,
  kinds, and pages; `--entry <name>` selects one `.class` for `inspect`,
  `methods`, or `disasm`. Entry matching is exact and case-sensitive, extraction
  is in memory (16 MiB limit), and ZIP64/encryption are rejected. Output for an
  entry reports `inputPath` as `app.jar!/pkg/Name.class`.

No DEX/APK, ELF/Mach-O/PE, SSA/data flow, class execution, or Java source
decompilation is implemented (decompilation is planned in [docs/decompiler.md](docs/decompiler.md)). This tool does not perform full JVM stack/type
verification. Uninterpreted attributes retain exact spans rather than invented
semantic results.

## Build, run, and test

Moss lives in the Joyeer repository and is built by the build-tree
[fern](../../docs/impl/fern.md) with the build-tree compiler. It needs the
optional `Compression` module for ZIP/JAR input, so configure with
`JOYEER_BUILD_COMPRESSION` (on by default; keep the defaults
`JOYEER_BUILD_FERN=ON` and `JOYEER_BUILD_MOSS=ON`):

```text
cmake -S . -B build -G Ninja
cmake --build build --target moss        # build/bin/moss
ctest --test-dir build -L moss --output-on-failure
```

Moss is an ordinary fern project, so once the compiler and fern exist you can
build it directly; `--bin-dir` places the executable in a bin folder:

```text
build/bin/fern build --compiler build/bin/joyeer --workspace workspace.toml     --module moss.app --bin-dir build/bin
build/bin/fern test  --compiler build/bin/joyeer --workspace workspace.toml --module moss.tests
```

`Moss.FernTests` runs the four native test modules with `fern test` and
`Moss.JavapGoldens` compares `moss disasm` of each checked-in class with its
`javap` golden byte for byte. Moss's modules are members of the repository's
root [workspace.toml](../../workspace.toml), so fern runs from the repository root
and test generations go to the root `target/` directory. `Compression` is not a member: modules declare it as a
dependency and the compiler resolves it from its standard library.

To keep disassembly in a file, give `--output` a new destination; it must not
already exist. File publication is fallible and exclusive, not crash-atomic.
Current Joyeer `print` cannot report stdout write/flush failures. Stdout is
fine for reading; use `--output` when delivery must be certain.

The optional JDK gates (`tests/jvm/integration.cjs --javap`, `corpus.cjs`) need
Node and an explicit JDK; they are not part of ctest.

## Project layout

| Area | Responsibility |
|---|---|
| [src/archive/](src/archive/) | ZIP/JAR entry listing and exact in-memory entry extraction |
| [src/binary/](src/binary/) | Checked coordinates/readers and bounded host acquisition |
| [src/model/](src/model/) | Format facts, instruction operands, IDs, diagnostics, byte/code-unit helpers |
| [src/jvm/](src/jvm/) | MUTF-8, descriptors, constant pool, class/attributes, and instruction decoder |
| [src/output/](src/output/) | `javap -c -p -l` text: declarations, generic signatures, constant comments, Java float/double text |
| [src/cli.joyeer](src/cli.joyeer), [src/cli/](src/cli/) | Options, exact selection, pages, query orchestration |
| [src/main.joyeer](src/main.joyeer) | Diagnostics, native exit boundary, exclusive output |
| [tests/](tests/) | Explicit native suites, self-authored binary fixtures, process integration |

All product code is Joyeer.

Source files import dependencies explicitly and use public short names for
types, constructors, and functions. Full module qualification is reserved for
ambiguity or intentional local shadowing; imports remain file-local.

Read-only string, container, and aggregate parameters explicitly declare
`borrowing`. This is equivalent to Joyeer's implicit default: callers retain
ownership, and argument passing does not deep-copy the input. Scalar
parameters keep the default spelling; mutation and ownership transfer use
`inout` and `consuming`, respectively.

## Validation

Moss prints `javap -c -p -l` text, so the main evidence is a byte comparison with
the real tool. On 2026-10-28, with Microsoft OpenJDK 25.0.4 `javap` and a native
Windows x64 Release build:

- The native suites passed: 673 binary checks, 1546 JVM checks (including 61
  `Double.toString`/`Float.toString` cases whose expected text came from the JDK
  itself), 33 CLI checks, and the archive suite.
- The [process integration suite](tests/jvm/integration.cjs) passed **779 checks**
  with `--javap`: the ASCII fixtures equal `javap -c -p -l` byte for byte, including
  all 217 opcode and wide encodings, plus Unicode and index selection, malformed
  mutations, exact pages, maximum Code length, budgets, and archive equivalence.
- The [corpus gate](tests/jvm/corpus.cjs) compared whole-class output with javap on
  real javac classes: all 57 classes of JDK 25 `jrt-fs.jar` and 400 classes of JDK 8
  `jsse.jar` were identical, and 200 `jsse.jar` classes also passed the
  control-flow-graph invariants and independent dominator/loop oracle for 867 methods.

No fixture was copied from another project; the corpora are JDK files used read-only
and are not part of the repository.

Run the integration suite against a built executable:

```text
node tools/moss/tests/jvm/integration.cjs build/bin/moss
```

Node uses only its standard library and is a development/test prerequisite,
not a product dependency. Durable class fixtures are checked in, so ctest does
not require Node or a JDK.

The optional javap gate needs an explicit JDK and is not run by ctest:

```text
node tools/moss/tests/jvm/integration.cjs build/bin/moss --javap <jdk>/bin/javap
```

The script compares javap method-descriptor instruction offsets/mnemonics.
Real javac-generated class coverage and deeper differential facts remain
release-hardening work. Self-authored structural fixtures are not proof of
executable stack correctness.

## Redistribution

No license has been selected for this project. Do not assume licensing or
fixture permissions transfer from earlier Moss work. Resolve project licensing,
third-party notices, and native toolchain/product inventories before release.
