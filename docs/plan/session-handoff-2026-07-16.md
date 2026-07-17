# Session Handoff — Native-only migration and full debug

**Date:** 2026-07-16

**Branch:** `feature/init_version`

**Pushed code baseline:** `7052035 feat(ir): carry source variables and scopes`

**Owner/commit identity:** `Qing Xu <xuqing@decompile.io>`

---

## 1. Executive summary

The user made a new authoritative decision:

> The old implementation is no longer needed. Stop maintaining legacy
> compatibility. Delete the legacy parser/AST passes, bytecode runtime, VM,
> legacy CLI mode, and legacy golden tests. Make the typed Joyeer IR/LLVM/native
> pipeline the only/default compiler pipeline.

Do **not** spend time repairing legacy-only assertions or preserving old output.
The immediate migration goal is to remove every path that can enter the old
`SyntaxParser -> TypeGen -> TypeBinding -> IRGen -> VM` lane.

There is also valuable uncommitted `-gfull` work in the working tree. Preserve
it. It adds lexical scopes, variables, physical debug types, and
`llvm.dbg.declare` on top of the already committed debug-location transport.
It is not yet committed or fully regression-tested.

---

## 2. Why the Microsoft Visual C++ Runtime dialog appears

Do **not** run this command on Windows before legacy removal:

```pwsh
ctest --test-dir build --output-on-failure
```

Unfiltered CTest registers 35 old golden tests from
[`tests/CMakeLists.txt`](../../tests/CMakeLists.txt). They invoke the compiler
without `--lang=v0.1`, so they enter the default legacy VM pipeline.

The deterministic reproducer is:

- test: [`tests/basis/array_01.joyeer`](../../tests/basis/array_01.joyeer)
- old stage: `TypeBinding::visitArrayFuncCallExpr()`
- assertion: [`lib/compiler/typebinding.cpp`](../../lib/compiler/typebinding.cpp#L186)
- statement: `assert(false)`

Archived CTest logs show this test blocking for 365, 846, and 893 seconds while
the CRT modal waited for input. The screenshot in the previous session is the
same `abort() has been called` dialog.

If the dialog is already open, choose **Abort** (not Retry), then stop any
remaining unfiltered CTest process. Do not diagnose or fix this assertion; the
entire owning lane is scheduled for deletion.

The native runtime is not the source of this modal. Its fatal path in
[`lib/native/runtime.c`](../../lib/native/runtime.c#L71-L76) flushes stderr and
uses C11 `_Exit(EXIT_FAILURE)`, specifically avoiding CRT abort dialogs.

---

## 3. Safe build and test commands

Until legacy tests and targets are gone, use only the bounded native/frontend
suite:

```pwsh
cmake -S . -B build -G Ninja \
  -DJOYEER_BUILD_UNITTESTS=ON \
  -DJOYEER_CLANG_EXECUTABLE='C:/Program Files/LLVM/bin/clang.exe'
cmake --build build
ctest --test-dir build \
  -L "diagnostics|fix-it|lexer|parser|name-resolution|type-checking|semantic-analysis|ir|ir-lowering|llvm-backend|native-runtime|native|ownership|file-io|array|dictionary|byte-conversion|json-parser|optimization|safety|consuming|initializing|exclusivity|projection-consume|debug-info|security" \
  --output-on-failure
```

Do not use `-R` patterns that accidentally select paths under `tests/basis`,
`tests/errors`, or `tests/leetcode`. Those tests themselves do not pass
`--lang=v0.1`; use only dedicated native/frontend CMake-script tests.

After legacy test registration is deleted, unfiltered CTest should become safe
again and should be restored as the final gate.

---

## 4. Repository state at handoff

### Committed and pushed

The branch and origin were synchronized at the start of the handoff:

```text
7052035 feat(ir): carry source variables and scopes
447097d feat(native): retain debug artifacts safely
b9c5e43 feat(cli): expose debug line tables
fb39af8 feat(llvm): emit source line tables
9bb7867 feat(ir): carry verified source locations
```

Important completed work:

- typed v0.1 frontend through native executable generation;
- complete JSON-parser native acceptance fixture;
- recursive clone/destroy and deterministic cleanup;
- all four ownership effects, projection consumption, and exclusivity;
- rich diagnostics with fix-its and secondary notes;
- verified source maps and source locations in Joyeer IR;
- DWARF 4 / CodeView line tables;
- CLI `-g0`, `-g`, `-gline-tables-only`, `-gdwarf`, `-gcodeview`;
- safe PDB/DWARF/dSYM artifact policy;
- backend-neutral lexical scopes, source variables, and storage-binding events.

The variable/scope transport commit passed 112 affected
`ir|ir-lowering|llvm-backend|json-parser` tests. The immediately preceding
native debug-artifact milestone passed the complete bounded 320-test suite.

### Uncommitted `-gfull` work — do not discard

At handoff, these files are intentionally dirty:

```text
M include/joyeer/backend/llvm.h
M include/joyeer/compiler/options.h
M lib/backend/linker.cpp
M lib/backend/llvm.cpp
M lib/compiler/compiler+service.cpp
M lib/runtime/arguments.cpp
M scripts/cmake/verifyNativeExecutable.cmake
M unittests/compiler/CMakeLists.txt
M unittests/compiler/llvm_test.cpp
M unittests/runtime/arguments_test.cpp
?? tests/native/full_debug.joyeer
```

The work currently implements:

- `DebugInfoOptions::emitVariables`;
- `EmitOptions::emitVariables`;
- new `-gfull` CLI mode while preserving `-g` as line-tables-only;
- full-debug compile units (`emissionKind: FullDebug`);
- physical primitive/opaque aggregate DI types;
- per-function `DISubroutineType`;
- `DILexicalBlock` from verified Joyeer IR scopes;
- `DILocalVariable` for parameters, locals, and match pattern bindings;
- portable legacy-form `llvm.dbg.declare` (not `#dbg_declare`);
- native/PDB integration tests and fixture
  [`tests/native/full_debug.joyeer`](../../tests/native/full_debug.joyeer).

Validated during the interrupted session:

- focused CLI/full-debug unit tests: 4/4 passed;
- real `--emit-llvm -O0 -gfull -gdwarf` output was accepted by Clang 22;
- direct CodeView object inspection showed `S_LOCAL` records for `plain`,
  `target`, `payload`, and locals, plus a `Pair` type record;
- final PDB inspection showed source files, local symbols, and type records;
- the native full-debug fixture prints:

```text
3
7
5
5
```

The latest full-debug integration CTest initially failed only because its PDB
assertions used `llvm-readobj` spelling (`LocalSym` / `VarName`) while the test
script invokes `llvm-pdbutil` (`S_LOCAL` / backtick names). The CMake patterns
were corrected, and the four focused full-debug tests then passed.

**Still required before committing this WIP:**

1. review the current diff;
2. run the bounded suite above after legacy-test registration is disabled or
   with the label filter;
3. update README/native/roadmap docs for `-gfull`;
4. commit as a separate feature, suggested message:
   `feat(debug): emit source variables and scopes`.

---

## 5. Native-only migration plan

Make this the first task in the new session. Prefer small, independently
buildable commits.

### Commit A — stop running legacy and make typed native default

1. Remove `LanguageMode::legacy` and `--lang=v0.1-legacy`.
2. Make the current typed pipeline unconditional/default.
3. Keep `--lang=v0.1` temporarily as an accepted no-op if desired for script
   migration, then remove it in a later cleanup.
4. Remove VM construction/bootstrap/run from
   [`lib/main/driver.cpp`](../../lib/main/driver.cpp).
5. Remove the legacy `else` body from
   [`CompilerService::compile()`](../../lib/compiler/compiler+service.cpp).
6. Delete legacy golden registration in
   [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) immediately. This removes
   the CRT dialog before deeper source deletion.

Exit criterion: a clean configure/build plus unfiltered CTest cannot launch
`TypeBinding` or the VM.

### Commit B — decouple the new frontend from legacy infrastructure

The new lexer still takes `CompileContext`, and `SourceFile` still includes old
node/runtime headers. Remove those accidental dependencies before deleting the
old libraries:

1. Change `LexParser` to accept `Diagnostics*` (or a lean frontend context)
   instead of legacy `CompileContext`.
2. Update direct lexer/parser/name/type/semantic/lowering tests accordingly.
3. Make `SourceFile` include only filesystem/token/native pipeline types;
   remove `ModuleClass* moduleClass`.
4. Shrink or delete `CompileContext`; it currently exists primarily for old
   AST passes and symbol/type stacks.
5. Move CLI argument parsing out of the legacy `JoyeerRuntime` target (for
   example into `lib/main` or a small CLI library).

Exit criterion: `JoyeerCompiler` and compiler unit tests no longer link
`JoyeerVM` or legacy `JoyeerRuntime`.

### Commit C — delete dead compiler/runtime/VM sources

Delete the old lane rather than leaving it unbuilt:

```text
include/joyeer/vm/
lib/vm/
include/joyeer/runtime/bytecode.h and other VM-only runtime headers
lib/runtime/bytecode.cpp
lib/runtime/descriptor.cpp
lib/runtime/gc.cpp
lib/runtime/heaps.cpp
lib/runtime/isolate+vm.cpp
lib/runtime/loader.cpp
lib/runtime/memory.cpp
lib/runtime/res+table.cpp
lib/runtime/sys.cpp
lib/runtime/types.cpp
include/joyeer/compiler/node.h
include/joyeer/compiler/node+visitor.h
include/joyeer/compiler/syntaxparser.h
include/joyeer/compiler/typegen.h
include/joyeer/compiler/typebinding.h
include/joyeer/compiler/IRGen.h
include/joyeer/compiler/debugprinter.h
lib/compiler/node.cpp
lib/compiler/node+visitor.cpp
lib/compiler/syntaxparser.cpp
lib/compiler/typegen.cpp
lib/compiler/typebinding.cpp
lib/compiler/IRGen.cpp
lib/compiler/debugprinter.cpp
```

Also remove the corresponding CMake targets/sources and old bootstrap APIs from
`CompilerService`.

Do **not** delete [`lib/native/`](../../lib/native) or
`JoyeerNativeRuntime`; that is the active native C runtime.

### Commit D — remove/archive legacy tests and stale docs

- Delete the old `basis/errors/leetcode` golden corpus and
  [`tests/testRunner.py`](../../tests/testRunner.py), except source fixtures
  still referenced by new CLI tests.
- Search [`unittests/compiler/CMakeLists.txt`](../../unittests/compiler/CMakeLists.txt)
  before deleting a fixture. For example, old-looking lexer error sources may
  still be reused by structured diagnostic tests; move them to a native
  frontend fixture folder if needed.
- Remove legacy-only runtime/unit tests and `JoyeerVM` link dependencies.
- Update README, docs index, parser/lexer/diagnostics docs, and roadmap so they
  describe one compiler pipeline, not a compatibility lane.
- Move historical bytecode documentation to an archive or delete it.

Exit criterion: repository search finds no active `LanguageMode::legacy`,
`SyntaxParser`, `TypeGen`, `TypeBinding`, `IRGen`, `JoyeerVM`, or old golden
runner references.

---

## 6. Recommended test strategy during deletion

After each commit:

```pwsh
cmake -S . -B build -G Ninja \
  -DJOYEER_BUILD_UNITTESTS=ON \
  -DJOYEER_CLANG_EXECUTABLE='C:/Program Files/LLVM/bin/clang.exe'
cmake --build build
ctest --test-dir build \
  -L "lexer|parser|name-resolution|type-checking|semantic-analysis|ir|ir-lowering|llvm-backend|native-runtime|native|diagnostics|debug-info|security" \
  --output-on-failure
```

Once [`tests/CMakeLists.txt`](../../tests/CMakeLists.txt) no longer registers
legacy goldens and the old targets are gone, run:

```pwsh
ctest --test-dir build --output-on-failure
```

That unfiltered command becoming safe is a migration acceptance criterion.

---

## 7. Things not to do

- Do not fix [`lib/compiler/typebinding.cpp`](../../lib/compiler/typebinding.cpp#L186).
- Do not preserve the VM to keep old golden output.
- Do not add new bytecode opcodes.
- Do not route new syntax through the old AST.
- Do not discard the dirty `-gfull` changes.
- Do not run unfiltered CTest until legacy test registration is removed.
- Do not use Rust/Cargo; the repository remains CMake + C++20 + C11 runtime.

---

## 8. Suggested first prompt for the new session

> Read `AGENTS.md` and `docs/plan/session-handoff-2026-07-16.md`. Preserve the
> dirty `-gfull` work. First remove legacy golden-test registration and make the
> typed Joyeer IR/LLVM/native pipeline unconditional/default. Then decouple the
> new lexer/source/CLI from legacy runtime and delete the old parser/AST passes,
> bytecode runtime, and VM in small committed/pushed steps. Never run unfiltered
> CTest until the legacy tests are removed.
