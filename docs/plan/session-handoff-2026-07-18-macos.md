# macOS Session Handoff

**Prepared:** 2026-07-18

**Branch:** `feature/init_version`

**Windows validation baseline:** `8ab4e0c`

**Commit identity:** `Qing Xu <xuqing@decompile.io>`

## 1. Repository state

The native-only migration is complete and pushed. Joyeer has one compiler
pipeline:

```text
source -> lexer -> parser -> name resolution -> type checking
       -> semantic analysis -> verified Joyeer IR -> textual LLVM IR
       -> Clang + JoyeerNativeRuntime -> native executable
```

The legacy parser/AST passes, bytecode runtime, VM, `--lang` modes, golden
corpus, and Python runner have been removed. Do not recreate a compatibility
lane.

The last Windows gate passed 328/328 tests, including native JSON parsing,
ownership, PDB, DWARF, and `-gfull`. macOS registers a different platform test
set: Windows PDB tests are absent and dSYM tests are present, so require 100%
pass rather than expecting the Windows test count.

## 2. Clone or resume on the Mac

For a new checkout:

```sh
git clone --branch feature/init_version --single-branch \
  https://github.com/joyeer/joyeer-lang.git
cd joyeer-lang
```

For an existing checkout:

```sh
git fetch origin --prune
git switch feature/init_version
git pull --ff-only
```

Set and verify the commit identity locally:

```sh
git config user.name "Qing Xu"
git config user.email "xuqing@decompile.io"
git config user.name
git config user.email
git status --short --branch
git log -1 --format='%H %an <%ae> %s'
```

The branch should be clean and track `origin/feature/init_version`. Do not copy
the Windows `build/` directory to the Mac; configure a fresh build tree.

## 3. Install macOS dependencies

Install the Xcode command-line tools if they are not already present:

```sh
xcode-select -p || xcode-select --install
```

Install the build and LLVM tools with Homebrew:

```sh
brew update
brew install cmake ninja llvm
```

Homebrew LLVM is keg-only. Resolve its location instead of hard-coding Intel
or Apple Silicon prefixes:

```sh
LLVM_PREFIX="$(brew --prefix llvm)"
"$LLVM_PREFIX/bin/clang" --version
"$LLVM_PREFIX/bin/llvm-readobj" --version
xcrun --find dsymutil
xcrun --show-sdk-path
uname -m
```

Expected prefixes are normally `/opt/homebrew/opt/llvm` on Apple Silicon and
`/usr/local/opt/llvm` on Intel, but `brew --prefix llvm` is authoritative.

## 4. Configure, build, and test

Use the platform compiler selected by CMake for the C/C++ implementation and
pass Homebrew Clang explicitly for generated LLVM IR and native linking:

```sh
LLVM_PREFIX="$(brew --prefix llvm)"

cmake -S . -B build -G Ninja \
  -DJOYEER_BUILD_UNITTESTS=ON \
  -DJOYEER_CLANG_EXECUTABLE="$LLVM_PREFIX/bin/clang"

cmake --build build
ctest --test-dir build --output-on-failure
```

The configure output should contain `Joyeer LLVM driver:` with the Homebrew
path. If Clang is not detected, delete only the new Mac build directory and
reconfigure with the explicit path above.

Unfiltered CTest is the required gate. Useful focused checks are:

```sh
ctest --test-dir build -L lexer --output-on-failure
ctest --test-dir build -L llvm-backend --output-on-failure
ctest --test-dir build -L native --output-on-failure
ctest --test-dir build -L debug-info --output-on-failure
```

## 5. macOS native/debug smoke test

Build a full-debug executable through Joyeer:

```sh
./build/bin/joyeer \
  -O0 -gfull -gdwarf \
  -o ./build/joyeer-full-debug \
  ./tests/native/full_debug.joyeer

./build/joyeer-full-debug
test -d ./build/joyeer-full-debug.dSYM
```

Expected program output:

```text
3
7
5
5
```

The macOS debug integration tests inspect `__debug_line` and the sibling dSYM.
`-gcodeview` is Windows-only and should be rejected on macOS.

## 6. Platform-specific checks

Pay attention to these areas when making the first Mac changes:

- dSYM creation, replacement, and stale-artifact cleanup;
- `llvm-readobj` Mach-O section spelling (`__debug_line`);
- Apple Silicon versus Intel process architecture;
- filesystem case sensitivity and path normalization;
- executable suffix and shell-metacharacter path tests;
- binary-safe `readFile(path:)` behavior and allocation balance.

Do not weaken a cross-platform test merely because the Mac tool spelling or
artifact layout differs. Keep platform adaptation in CMake/test helpers and
preserve the common compiler behavior.

## 7. Current next work

Use [roadmap.md](roadmap.md) as the active plan. The highest-value remaining
areas are:

1. reconcile the normative copy/deinitialization model for heap-backed values;
2. broaden the standard library and typed I/O surface;
3. define Joyeer-specific optimization/LTO policy without weakening safety;
4. improve optimized debugger inspection;
5. expand modules, generics, errors, and contracts only with end-to-end tests.

The implementation boundary is documented in
[../impl/native.md](../impl/native.md). Extend Joyeer IR, LLVM lowering, or the
native runtime rather than introducing another execution pipeline.

## 8. Before handing work back

```sh
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
git status --short --branch
git log -1 --format='%H %an <%ae> %s'
git push origin feature/init_version
```

Record the Mac model, CPU architecture, macOS version, Xcode/CLT version,
Homebrew LLVM version, exact test count, failures or skips, and the pushed
commit SHA in the next handoff.