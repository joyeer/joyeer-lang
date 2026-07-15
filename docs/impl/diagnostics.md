# Diagnostics — Structured v0.1 Source Rendering

> **Status:** Stable IDs, severity, file paths, one-based locations, source
> excerpts, caret ranges, structured help, and applicable source edits are
> implemented for the `--lang=v0.1` pipeline. Legacy VM diagnostics retain
> their historical output format.

---

## 1. Model and compatibility boundary

`Diagnostics` remains the shared carrier in
`include/joyeer/diagnostic/diagnostic.h`. An `ErrorMessage` may now contain:

- `ErrorLevel` (`failure` or nonfatal `report`);
- a stable stage-qualified code;
- a source path;
- internal zero-based line/byte-column and span length;
- the source line used for rendering;
- optional help text and one optional byte-based replacement edit.

Existing `reportError()` calls create compatibility diagnostics and continue to
print as `SyntaxError(line: ...)` or `Warning(line: ...)`. This preserves the
legacy parser/VM golden corpus. New frontend stages call
`reportSourceDiagnostic()`; source-independent failures such as linker errors
call `reportDiagnostic()`.

---

## 2. v0.1 output contract

A source diagnostic is rendered as:

```text
file.joyeer:2:12: error[type-checking.type-mismatch]: cannot use value of type 'String' where 'Int' is required
  2 |     return "text"
    |            ^~~~~~
```

The contract is:

- file, line, and column lead the first line for editor/CI recognition;
- displayed line and column are one-based;
- severity is `error` or `warning`;
- the stable ID remains stage-qualified (`lexer.*`, `parser.*`,
  `name-resolution.*`, `type-checking.*`, `semantic-analysis.*`,
  `ir-lowering.*`, `llvm.*`, or `linker.*`);
- the source excerpt is one physical line;
- `^` identifies the first byte and `~` covers the remainder of the span;
- tabs before the caret expand to four-column tab stops.
- optional `help:` lines explain a recovery;
- optional `fix-it:` lines encode `path:line:column:byte-length: "replacement"`.

Internal `SourceSpan` offsets remain zero-based UTF-8 byte offsets. Rendering
never changes the semantic span model.

---

## 3. Pipeline integration

The v0.1 lexer maps every lexical failure category to a stable `lexer.*` ID and
reports the consumed source span. Parser, name-resolution, type-checking,
semantic-analysis, IR-lowering, and LLVM diagnostics already carry IDs and
spans; `CompilerService` forwards those fields separately rather than joining
the ID into the message.

Warnings are rendered through the same path but do not set `hasFailure()` and
do not stop LLVM/native output. The driver prints warnings once after a
successful link, or prints accumulated diagnostics once on failure.

Access-effect marker mismatches provide source edits to insert, replace, or
remove `&` / `consume`. Ownership-flow diagnostics provide help for
reinitialization and all-path initializing obligations. Parser diagnostics
provide zero-length insertion edits for missing canonical punctuation,
delimiters, arrows, and list commas. Ordinary type mismatches provide explicit
`expected` / `found` help while retaining the stable primary message and ID.
Offsets remain byte based so editor tooling can apply edits without
reinterpreting display width.

---

## 4. Remaining work

- context-aware parser/type replacement and deletion edits;
- secondary note locations for diagnostics involving multiple source spans;
- multi-line span rendering;
- Unicode display-column calculation (current columns are UTF-8 byte columns);
- migration of the compatibility parser/VM diagnostics to stable IDs;
- machine-readable JSON/SARIF output.

These additions should extend the structured model without changing existing
stable IDs.

---

## 5. Validation

```pwsh
ctest --test-dir build -L diagnostics --output-on-failure
```

Tests cover failure/warning severity, source-independent structured errors,
exact one-based source rendering, help/fix-it escaping, and CLI output from the
lexer, parser, and type checker. CLI cases include an applicable missing-`)`
insertion, a `consume` insertion, and expected/found type guidance. Existing
legacy formatting tests ensure the compatibility path does not change.
