---
name: moss
description: Disassemble Java .class files and JARs with the moss CLI - list JAR entries, list methods, and print JVM bytecode in the same layout as `javap -c -p`. Use when asked to analyze, disassemble, or understand compiled Java/JVM code, a .jar, or a .class file. Does not produce Java source.
---

# moss

`moss` is a command-line tool. This skill bundles the executable at
`bin/moss` (`bin/moss.exe` on Windows) next to this file; run that path, or
plain `moss` if it is on `PATH`. Check with `<path> --version`. Examples below
write `moss` for whichever you use. It reads files only, never executes
classes, and prints bounded output, so it is safe to run on untrusted input.

## Workflow for a JAR

1. Find the class: `moss entries app.jar --prefix com/acme/` lists entry names
   under a package, one per line (add `--offset N --limit N` to page). Without
   `--prefix` it lists everything.
2. Read a whole class: `moss disasm app.jar --class com.acme.Foo` prints every
   method's bytecode. `--class` takes `com.acme.Foo` or `com/acme/Foo`
   (a trailing `.class` is fine); use `--entry <exact name>` for anything else.
3. Narrow down when a class is large: `moss methods app.jar --class com.acme.Foo`
   lists the method declarations, each with its index and descriptor, then
   `moss disasm app.jar --class com.acme.Foo --method run` shows one. Add
   `--descriptor "(II)I"` when the name is overloaded (the error
   `query.ambiguous-method` says so), or use `--method-index N`.

For a standalone class, drop the archive and selector: `moss disasm Foo.class`.
`moss inspect <class>` adds the version, the constant pool, fields, and method
declarations without bytecode.

## Output format

The output is plain text in the same layout as `javap -c -p -l` (JDK 25): a
`Compiled from` line, the class declaration with generics, fields, and each
method with its `Code:` listing. Read it exactly as you would read javap output:

```text
  public static int sum(int, int);
    Code:
         0: iload_0
         1: iload_1
         2: iadd
         3: ireturn
```

- Constant-pool operands carry javap's `//` comment, for example
  `invokevirtual #13  // Method java/io/PrintStream.println:(Ljava/lang/String;)V`.
  Branch targets are absolute offsets. `Exception table`, `LineNumberTable`, and
  `LocalVariableTable` follow the code when present. Wide forms print as `iload_w`.
- Non-ASCII text is printed as `\uXXXX` (javap prints `?`), so names and string
  constants stay exact. A `float`/`double` constant prints as Java would.
- Output goes to stdout. To save it, add `--output <new-file>`; the file must not
  exist (moss never overwrites).
- Long results are paged. A trailing `// more: continue with --offset N` line means
  there is more: rerun with that `--offset`. The offset is a method index for
  `methods` and whole-class `disasm`, a bytecode offset for single-method
  `disasm`, and a position among the selected names for `entries`.
- Whole-class output over 8 MiB fails with `output.budget-exceeded`; page it
  with `--limit N` (methods per page) or disassemble single methods.

## Control flow

`moss cfg app.jar --class com.acme.Foo --method run` prints the basic blocks of one
method with their predecessors and successors (branch, fallthrough, switch case,
and exception edges), each block's immediate dominator, and the natural loops
(header, latches, blocks, depth). `Irreducible edges` means some cycle has several
entries and no loop is reported for it. It is structural only: it does not prove
the code is valid or recover source-level conditions.

## Trust and limits

- Bytecode listings are exact structural decoding, not decompilation and not
  JVM verification. Do not present them as Java source or claim the code is
  valid. Describe behavior from the instructions you actually see.
- Supports Java 8-25 class files and classic ZIP/JARs (STORE/DEFLATE). Input
  and each entry are limited to 16 MiB. Nested JARs, DEX/APK, ZIP64 and
  encrypted archives are not supported: extract a nested JAR first.
- Annotations and attributes other than those javap -c -p -l prints are not shown.
- A nonzero exit prints `moss: <code> [...]` on stderr, for example
  `archive.entry-not-found` or `jvm.invalid-magic`. Fix the argument named by
  the code instead of retrying unchanged.
