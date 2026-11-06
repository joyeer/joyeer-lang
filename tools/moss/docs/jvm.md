# JVM CLI and Output Contract

## Status and scope

Single-class inspection, method enumeration, whole-class and per-method
bytecode disassembly in `javap -c -p -l` layout, and ZIP/JAR entry listing and
selection are implemented. The structural policy accepts major 52..69
(Java 8..25), minor 0. Major 69/minor 65535 is accepted only with
`--allow-preview`; earlier preview versions reject even with that flag.

The output is plain text only. There is no JSON or NDJSON format.

This is an early-development implementation verified natively on Windows
(ARM64 and x64) and, for the sources, on Linux x64. Disassembly was compared
byte for byte with `javap -c -p -l` from JDK 25 (see Verification). Other native
platforms remain unverified. Version parsing is not proof that every language
construct produced by every compiler has been exercised.

No analyzed code is executed. No classpath is fetched or resolved. The tool
does not perform full stack/type verification, resolve member linkage,
reconstruct Java source, or interpret every optional attribute.

## Commands

```text
moss --help
moss --version
moss serve
moss entries <archive.jar|.zip> [--prefix <path>] [--offset <n>] [--limit <count>] [options]
moss cfg <input> (--method <name> [--descriptor <descriptor>] | --method-index <index>) [--offset <block>] [--limit <count>] [options]
moss inspect <input> [options]
moss methods <input> [--offset <index>] [--limit <count>] [options]
moss disasm <input> [options]                                    # every method
moss disasm <input> --method <name> [--descriptor <descriptor>] [options]
moss disasm <input> --method-index <index> [options]
```

`<input>` is a `.class` file, or an archive path plus `--entry <name>` or
`--class <name>`.

Options:

| Option | Contract |
|---|---|
| `--output <new-file>` | Write the text to a new file instead of stdout; exclusive, never replaces an existing file |
| `--allow-preview` | Allow only Java 25 preview |
| `--entry <name>` | Select one exact, case-sensitive archive entry as the class; the input is then a ZIP/JAR |
| `--class <name>` | `com.acme.Foo`, `com/acme/Foo`, or `com/acme/Foo.class`; same as `--entry com/acme/Foo.class` |
| `--prefix <path>` | `entries` only: keep names that start with this exact, literal path prefix |
| `--method <name>` | Exact case-sensitive UTF-16 name matching from UTF-8 argv |
| `--descriptor <descriptor>` | Exact descriptor; only with a named method selector |
| `--method-index <index>` | Zero-based physical method inventory index, instead of a name |
| `--offset <number>` | Methods: inventory index; single-method disasm: method-relative byte offset; entries: position among the selected names; cfg: block index |
| `--limit <number>` | 1..10000 records, default 1000 |

The input path immediately follows the command. Options use separate
nonempty values. Singleton duplicates, extra positional arguments, unknown
options (including the removed `--format` and `--unchecked-stdout`),
unsupported command combinations, negative/out-of-range integers, and numeric
overflow fail with `cli.usage`. Help/version must be used alone.

`inspect` is a complete class inventory and does not accept page options or
method selectors. `methods` pages method declarations and does not accept
method selectors. `disasm` with a method selector selects exactly one method
and pages its instructions. `disasm` without a selector prints every method
(including methods without Code, which have no instructions) and pages by
method index: `--offset` is a method index and `--limit` counts methods.
`--descriptor` still requires `--method`.

### Control-flow graph

`cfg` builds the basic-block graph of one method with bytecode. It requires a
method selector (`--method`/`--method-index`); an abstract or native method fails
with `query.no-code`. Blocks are maximal single-entry, single-exit instruction
runs, ordered by start offset. Leaders are offset 0, every branch/switch target,
the instruction after any branch, switch, return or throw, and every exception
range start, end and handler.

```text
Class pkg.Example
Method loop (I)I
Cfg blocks 3 edges 3 unreachable 0 loops 1 irreducible-edges 0
Block 0 [0,4) instructions 0+2 reachable terminator=conditional preds=[1] idom=none loopDepth=1
  -> 2 branch
  -> 1 fallthrough
Block 1 [4,8) instructions 2+2 reachable terminator=goto preds=[0] idom=0 loopDepth=1
  -> 0 jump
Block 2 [8,9) instructions 4+1 reachable terminator=return preds=[0] idom=0 loopDepth=0
Loop header=0 latches=[1] blocks=[0,1] depth=1
```

A block line gives its method-relative range (end exclusive), the index and count
of its first instruction, `terminator` (`fallthrough`, `goto`, `conditional`,
`switch`, `return`, `throw`), reachability (`UNREACHABLE` otherwise), predecessors,
`idom`, and `loopDepth`. A successor line is `-> to kind` with kind `fallthrough`,
`jump`, `branch` (taken conditional), `case` (followed by `key=`), `default`, or
`exception` (followed by `catch=` with the class name, or `catch=any`). Exception
edges are listed after normal edges, in exception-table order, from every block
inside the protected range. Reachability follows normal and exception edges from
block 0. Paging is by block index; the `Loop` and `Irreducible edges` lines are
always complete.

Dominance and loops treat every edge, exception edges included, as control flow.
`idom` is the immediate dominator (`none` for the entry and unreachable blocks) and
`loopDepth` is the number of natural loops containing the block. A *back edge* is an
edge whose target dominates its source. Each header yields one natural loop
(`blocks` ascending and including the header; `depth` is the header's `loopDepth`).
A retreating edge of the depth-first search that is not a back edge makes the
method **irreducible**: such edges are listed on the `Irreducible edges` line
(`from->to`) and no natural loop is invented for them. This is structural control
flow only: it does not verify stack depths or types, and it does not model
`jsr`/`ret` (rejected earlier).

### Archives

`entries` lists a classic single-disk ZIP/JAR (STORE and DEFLATE; no ZIP64 or
encryption) from its central directory, one entry name per line in archive
order. Paging indexes the selected (prefix-filtered) names. No entry kinds or
indexes are printed; a trailing slash marks a directory.

`--entry`/`--class` extract one entry in memory (16 MiB limit, CRC checked),
then run the ordinary class query on it; the output is the same as for the
extracted file. Duplicate entry names are rejected as ambiguous, and
`META-INF/versions/*` is never consulted implicitly. Passing a JAR without
`--entry`/`--class` fails with `input.archive-needs-entry`. Archive failures
use `archive.*` codes (`archive.invalid`, `archive.unsupported`,
`archive.entry-not-found`, `archive.ambiguous-entry`, `archive.limit-exceeded`).

Named overload ambiguity fails with `query.ambiguous-method`; no fallback
overload is chosen. Method index selection also handles code-unit names that
cannot be expressed in UTF-8 command-line arguments, such as isolated
surrogates or embedded NUL. Names containing those units remain lossless
in the output.

### Paging

For method enumeration, offsets range from zero through methodCount.
methodCount yields an empty terminal page. Pages remain in classfile order.

For single-method disassembly, an offset must be an instruction start, not a
byte in an operand, wide instruction, or switch table. The exact exclusive method
Code length is accepted as an empty terminal page. Native/abstract methods have
no Code and print only their declaration.

When more records follow, the text ends with a line

```text
// more: continue with --offset <n>
```

where `<n>` is the next coordinate: a method index, a byte offset, a block
index, or a position among the selected entry names. Without that line the page
reached the end. A paged single-method listing omits the exception, line-number,
and local-variable tables, which belong to the complete method. All selected
method bytecode is decoded/checked before a page is printed, so a malformed
instruction after the page boundary still fails.

## Persistent session (`moss serve`)

`moss serve` reads requests from stdin until `quit`, `exit`, or end of input
and keeps the most recent input in memory. It exists so an agent that asks many
questions about one large JAR or class does not re-read and re-parse it for
each question. It is a line protocol over stdin/stdout, not MCP and not JSON.

Request: one line, the ordinary arguments without the program name, for example
`disasm app.jar --class com.acme.Foo --method run`. Arguments split on spaces
and tabs. Double quotes group text, including spaces and empty arguments; inside
quotes a backslash escapes the next byte. Blank lines are ignored. A line may
hold at most 8192 bytes; a longer one gets an error reply and is skipped.
Besides the analysis commands, `--help`, `--version`, `reset`, `quit`, and `exit`
are accepted. `--output` is rejected (`serve.output-unsupported`) and a nested
`serve` is rejected (`serve.nested`).

Reply: a header line, then exactly that many lines.

```text
moss: ok <lines>
<lines of the same text the command line prints>
```

```text
moss: error <lines>
<code> [<context>]: <message>
```

The body of an `ok` reply equals the command's stdout without its final newline.
The header counts lines, not bytes, so a reader stays in step where stdout turns
LF into CRLF (Windows); strip a trailing CR from each line. An error reply never
ends the session, and a failed request leaves the cached input unchanged.
Errors are replies on stdout (not stderr) so one stream carries the whole
conversation. Stdin failure or a nonzero exit before the loop still reports on
stderr.

Cache: one input at a time, keyed by path. The first query on a path reads the
file (16 MiB limit); later queries on it do not read it again. The parsed class
is also kept for the same `--entry`/`--class` and `--allow-preview`; asking for
another entry or a different preview setting parses again without rereading.
Asking about a different path replaces the cache. Moss does not watch the file:
send `reset` after the file changes on disk. Peak memory is the input, the
selected entry, and one parsed class.

Replies are flushed before the next request is awaited. `readStdinLine` flushes
stdout first (spec §18.5), and `print` has no error path, so a write failure to
stdout is still undetectable.

## Parser and decoder behavior

The classfile reader checks:

- Magic, version/minor policy, counts, byte budgets, and final file position.
- Constant indexes, expected tags, modern tag/version constraints,
  MethodHandle references, dynamic bootstrap indexes, and dual-slot constants.
- Strict Modified UTF-8 byte encoding, preserving UTF-16 code units including
  surrogate pairs and isolated surrogates; no replacement decoding.
- Class/member names, descriptors, receiver/parameter slots, duplicate
  name+descriptor members, selected flag incompatibilities, and Code presence.
- Code length/maxLocals, attribute subregions, exception ranges, line/local
  tables, ConstantValue, Exceptions, SourceFile, BootstrapMethods, and
  Module/ModulePackages structure.

Other attributes, including StackMapTable, annotations, record metadata, and most
compiler-specific attributes, are not interpreted or printed. `Signature`
attributes of the class, its fields, and its methods are read for the generic
declarations javap shows. Known attributes handled by this reader must consume
exactly their declared body. The implementation is not a complete JVMS
access-flag/attribute verifier.

`inspect` and `methods` validate classfile metadata but do not decode every
method. `disasm` additionally validates the entire selected Code stream.

The decoder covers every permitted modern base opcode and all modern wide
forms, preserving raw/effective opcode, exact bytes/size, and signed operands.
`jsr`, `jsr_w`, `ret`, wide ret, reserved opcodes, malformed padding lengths,
or truncated operands reject. Switch padding values are not constrained;
alignment is relative to the method, not the file.

Static checks include constant/opcode compatibility, local slot width,
invokeinterface count and reserved bytes, invokedynamic reserved bytes,
array type/dimensions, return-descriptor compatibility, switch counts/ordering,
instruction-boundary branches, exception/debug boundaries, and final
fallthrough rejection. Runtime stack contents, hierarchy, and linkage are
not inferred.

## Output format

`disasm` follows `javap -c -p -l` of JDK 25. A class prints as

```text
Compiled from "Example.java"
public abstract class Example {
  public static final int value;

  public static int sum(int, int);
    Code:
         0: iload_0
         1: iload_1
         2: iadd
         3: ireturn
      LineNumberTable:
        line 4: 0
      LocalVariableTable:
        Start  Length  Slot  Name   Signature
            0       4     0     a   I
            0       4     1     b   I
}
```

- Declarations: modifiers in javap order (`default` for non-abstract interface
  instance methods), generic type parameters, parameter and return types in source
  form (`java.util.List<java.lang.String>`, `int...` for varargs), `throws`
  from the `Signature` or `Exceptions` attribute, constructors under the class name,
  and `static {};` for the class initializer. A blank line follows each field and
  separates methods. A class without a `Signature` joins its interfaces with `,`
  and no space, exactly like javap.
- Instructions: the offset right-aligned in a column that ends at column 10, then
  `: `, the mnemonic padded to 13 columns, and the operand. Constant-pool operands
  print `#index`, pad to column 46, and add `// comment`; the owner is left out when
  the reference names the class itself (same constant-pool entry, as javap does).
  `invokeinterface`, `invokedynamic`, and `multianewarray` print `#index,  n`. Branch
  operands are absolute offsets. `tableswitch` and `lookupswitch` print their keys
  and targets in a braced block with a `default:` line. Wide forms print as
  `iload_w`, `iinc_w`.
- Comments: `String` constants escape `\"`, `\'`, `\\`, `\n`, `\r`, `\t`, `\b`, `\f`
  and trim trailing blanks; `int`, `long` (`l`), `float` (`f`), `double` (`d`) print
  as Java's `toString` does, computed exactly with big integers (shortest
  round-trip form, `NaN`, `Infinity`, `-0.0`); array classes are quoted
  (`class "[[I"`); `MethodHandle` prints `REF_invokeStatic owner.name:descriptor`.
- Tables: `Exception table`, `LineNumberTable`, and `LocalVariableTable` appear
  after the code, in attribute order, with javap's column layout. Generic
  `LocalVariableTypeTable` rows are not printed (javap omits them too).

Differences from javap, all deliberate:

- Non-ASCII UTF-16 code units print as `\uXXXX`; javap prints `?` through the console
  code page. Names and strings therefore stay lossless.
- `methods` and `inspect` are Moss views (declarations with `// index N, descriptor D`,
  and version, flags, and constant pool), not javap's output.
- Output ends in exactly one LF; paged output ends with the `// more` line.

`entries` prints names, and `cfg` prints the block listing above.

## Budgets and failures

| Resource | Inclusive policy |
|---|---|
| Input | 16777216 bytes, acquired with one extra sentinel |
| Code | 1..65535 bytes |
| Requested page | 1..10000 records |
| Output | 8388608 bytes including final LF |
| Total Utf8 payloads | 1048576 encoded bytes before decoded-array allocation |
| Parsed metadata records | 262144 charged records |

Counts are validated against remaining bytes before iteration/allocation.
Metadata/text budgets are deliberate early-development domain limits;
an otherwise structurally valid class can reject with `jvm.resource-limit`.
A whole-class listing of a large class can exceed the output budget with
`output.budget-exceeded`; page it with `--limit` or select methods. No
truncation or success-shaped fallback is used.

The output ceiling includes exactly one LF. The byte ceiling is not a total
heap, time, or filesystem confinement guarantee.

Exit statuses:

- 0: successful command/query publication under the chosen delivery mode.
- 2: command-line usage/selection syntax error.
- 1: acquisition, malformed/unsupported class or code, query resolution,
  budget, output, or diagnostic-write failure.

Query lookup errors are domain status 1, not command syntax errors.
Stable diagnostic code, file offset when available, context, and message
are written to stderr. Native input/output error codes are retained in
messages. No stdout text is emitted after an analysis error.

`--output` uses exclusive `writeFileNew` and may leave a partial new file
after a host write/close failure; it is not atomic publication or durability.
Existing files are never replaced, including the input. No temp-directory
or artifact cleanup API is needed in the product.

Stdout uses the current unchecked `print`. Windows text-mode newline
translation and broken-pipe errors are outside its exact byte/success guarantee;
use `--output` for automated results.

## Verification and remaining gates

Native suites (`fern test`): 673 binary checks, 1546 JVM checks, 33 CLI checks,
and the archive suite. They include 61 `Double.toString`/`Float.toString` cases whose
expected text was produced by the JDK, the byte-exact layout of a `tableswitch`
method, and generic-signature parsing.

The process [integration suite](../tests/jvm/integration.cjs) compares `disasm` of the
ASCII fixtures with stored `javap -c -p -l` output
([`*.javap.txt`](../tests/fixtures/jvm/)), including all 217 opcode and wide encodings
in `OpcodeMatrix`, and covers every supported version number, malformed inputs,
Unicode and index selection, paging, exclusive writes, archive equivalence, and the
output ceiling. With `--javap <explicit javap>` it also compares against a live
javap.

The corpus gate [`tests/jvm/corpus.cjs`](../tests/jvm/corpus.cjs) compares Moss with
`javap -c -p -l` over real javac-compiled classes (class files extracted from
`java.base.jmod` or a JAR) and, with `--cfg`, checks every method's control-flow
graph against structural invariants and an independent set-based dominator and
natural-loop oracle.

Fixtures are [self-authored](../tests/fixtures/jvm/README.md). The native suite
truncates an entire example class at every byte position. The process suite adds
targeted malformed cases and deterministic mutations; this is not an exhaustive
fuzzing or security proof.

Known gaps against javap: annotations and other attributes javap prints only with
`-v` are not shown; classes older than major 52 are rejected by policy; non-ASCII
text is escaped instead of replaced.

Remaining release gates: broader native platforms, full peak/allocation
measurements, licensing, third-party/product inventories, and packaging.
Fallible stdout remains a Joyeer capability gap. DEX, native backends,
SSA, and decompilation remain separate later phases.
