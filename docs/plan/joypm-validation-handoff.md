# joypm Validation Handoff

**Status:** ready for validation on another machine, not release-complete.
This handoff records the source state and remaining execution work as of
2026-10-02. Remove or fold it into the owning implementation/build documents
when the handoff is complete; it is not another language specification.

## 1. What is ready

- The Joyeer-written local tool is under [src/tools/joypm/](../../src/tools/joypm/).
  `init/check/build/run/test`, strict schema-directed TOML parsing, deterministic
  explicit-module planning, and fresh output generations are implemented.
- CMake bootstraps `joypm` from the build-tree compiler. `joypm` and
  `JoypmAcceptanceTools` are default `ALL` targets when their options are enabled.
- `writeStderr` and bounded `readFilePrefix` have compiler/runtime plumbing and
  regression tests. Manifest acquisition accepts only ordinary regular entries
  and reads at most 65537 bytes; classification is not race-free confinement.
- Windows compiler arguments use strict UTF-16-to-UTF-8 conversion. Compiler
  and backend filesystem paths preserve UTF-8/native-path spelling. Legacy
  narrow `readFile` remains unchanged.
- Product installation includes optional joypm; Windows Debug also includes
  its PDB. The completed M0 plan was removed; current contracts live in the
  specification and implementation documentation.

Behavior, limits, and source responsibilities are in
[joypm implementation](../impl/joypm.md). Remaining product scope is in the
[package-manager plan](package-manager.md). This commit does not add dependencies,
workspaces, a registry, caching, `clean`, or automatic toolchain installation.

## 2. Evidence and architecture correction

The originating machine is **reported by its owner as Windows ARM64**. The
recorded build nevertheless used `vcvars64.bat`, the `x64-debug` preset, and an
x64 LLVM SDK. Treat the following as **x64-target build/smoke evidence only**,
not native ARM64 validation or proof of the physical host's architecture.
Architecture strings from an emulated shell or tool session are insufficient
to establish the native host. No ARM64 configuration was executed in this work.

| Check | Recorded result | Boundary |
|---|---|---|
| Debug default `ALL` build | Passed; 121 build steps with unit tests enabled | x64 target; includes joypm/bootstrap/helper compilation, not test execution |
| `joypm --help` / `--version` | Passed; version `joypm 0.1.0` | Startup smoke only |
| Fresh project with spaces and `雪🦀` in its path | `init/check/build/run/test` returned 0 | One project; `test` reported `0 tests`, so no declared test executable ran |
| Project Debug and Release profiles | Both built; `run` printed the scaffold greeting | Both used the Debug-built compiler/tool, not a Release toolchain bootstrap |
| Repeated Debug build | Produced `build-2` | Fresh generation smoke, not concurrent allocation or failed-rebuild coverage |
| CTest / nine joypm gates | **Not executed for this change set** | All acceptance assertions remain unverified |
| ARM64, Linux, macOS, packaged install, self-build | **Not executed** | No support/release claim from the above smoke |

The CMake editor integration had no active configure preset. Separate Developer
shell tasks configured and built successfully, but the editor build/test entry
could not discover the configured project. This was an execution blocker, not
a CTest failure. Old logs in an existing build tree are not evidence for this
change set. UTF-8 paths worked in the smoke; terminal glyph rendering was not
validated and is not proof that diagnostic display is Unicode-correct.

## 3. Set up the receiving machine

1. Check out the handoff commit and record `git rev-parse HEAD`. Transfer source
   through Git, not local binaries, build caches, generated platform files, or
   temporary fixture projects.
2. Determine the native Windows architecture independently of the terminal
   process. On Windows ARM64, select `arm64-debug` / `arm64-release` for native
   validation; x64 emulation is a separate test configuration.
3. Provide external prerequisites from [building](../building.md): CMake 3.20+,
   Ninja, MSVC target tools, Windows SDK, matching DIA SDK, and LLVM/LLD 22.1.8
   **development libraries** using `/MT`. Do not use the tool-only LLVM installer.
   Linux additionally requires the matching Clang Driver library.
4. Start a matching Visual Studio Developer shell. Use `vcvarsarm64.bat` for
   ARM64 or `vcvars64.bat` for x64. The MSVC target, LLVM SDK, compiler/backend,
   native runtime, and generated executables must all agree.
5. Use a fresh architecture-specific build directory. Never repoint an existing
   x64 cache to ARM64 or copy the originating machine's `out/` tree.

| Native Windows target | Presets | LLVM archive | DIA libraries |
|---|---|---|---|
| ARM64 | `arm64-debug`, `arm64-release` | `clang+llvm-22.1.8-aarch64-pc-windows-msvc.tar.xz` | `lib/arm64` |
| x64 | `x64-debug`, `x64-release` | `clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz` | `lib/amd64` |

For VS Code, select both the matching **Configure Preset** and **Build Preset**
in CMake Tools and configure the project. Running a shell configure task does
not automatically activate an editor preset. Tasks labeled `x64` are not native
architecture discovery. If editor preset selection remains unavailable, perform
the documented build/test workflow from the matching Developer shell instead;
do not mark an unstarted editor test invocation as a test result.

## 4. Execute Debug and Release acceptance

The example below is for **native Windows ARM64**, from an ARM64 Developer
PowerShell at the checkout root. Change `$architecture` to `x64` and provide
the matching SDK only when validating an x64 target.

```powershell
$architecture = 'arm64'
$llvmRoot = 'C:\LLVM-22.1.8-arm64'

foreach ($configuration in @('debug', 'release')) {
    $preset = "$architecture-$configuration"
    $buildDir = "out/build/$preset"

    cmake --preset $preset "-DJOYEER_LLVM_ROOT=$llvmRoot" `
        -DBUILD_TESTING=ON -DJOYEER_BUILD_JOYPM=ON `
        -DJOYEER_BUILD_UNITTESTS=ON -DINSTALL_GTEST=OFF
    if ($LASTEXITCODE -ne 0) { throw "Configure failed: $preset" }

    cmake --build --preset $preset
    if ($LASTEXITCODE -ne 0) { throw "Build failed: $preset" }

    ctest --preset $preset --output-on-failure `
        --output-log "$buildDir/ctest-handoff.log"
    if ($LASTEXITCODE -ne 0) { throw "CTest failed: $preset; preserve its logs" }
}
```

Build the default `ALL` target, **not only `joyeer`**: otherwise joypm or its
acceptance helpers can be missing. All three testing/tool options above must
remain enabled for the full gate. `INSTALL_GTEST=OFF` disables GoogleTest's
install rules, not its tests. Run unfiltered CTest as the final gate; use
`-R '^Joypm\.'` or labels only for focused failure iteration afterward.

Linux and macOS use their native Debug/Release presets and the same options;
follow [platform prerequisites and commands](../building.md#configure-build-and-test).
Debug project builds request `-gfull`; macOS needs `dsymutil` for those tests,
even though the tool bootstrap itself can fall back to `-g0` without it.
A Windows-only result is not evidence for either POSIX platform.

## 5. What to inspect and preserve

The nine gates are registered in
[tests/joypm/CMakeLists.txt](../../tests/joypm/CMakeLists.txt):

| Gate | Main checks |
|---|---|
| `Joypm.Cli` | Help/version, usage, duplicate/unknown flags, status and stream policy |
| `Joypm.Init` | Exclusive scaffolding, nonempty destinations, missing parents |
| `Joypm.Manifest` | Strict schema/profile, malformed input, UTF-8 and scalar escapes, cycles |
| `Joypm.ManifestLimits` | Exact input/string/module/target/array limits and source locations |
| `Joypm.Plans` | Deterministic explicit argv, module closures, target/profile selection |
| `Joypm.Workflow` | Both project profiles, generations, failed rebuilds, missing artifacts, run/cwd/argv |
| `Joypm.TestRunner` | Declared pass/build-fail/run-fail targets, continuation, serial summaries |
| `Joypm.Unicode` | Unicode/spaced paths and argument forwarding through compiler and runtime |
| `Joypm.Logic` | Direct Joyeer CLI/manifest logic entry |

Also inspect the full compiler/runtime suites for scalar host typing and ABI,
prefix-reader ownership/errors, binary stderr and closed-stream failures,
Windows wide CLI conversion, and Unicode backend paths. Registered fixtures
do not establish coverage of every release-gate case.

After a passing full Debug gate, run the Windows installer regressions with
the matching Debug tree, using
[test-install-debug.ps1](../../scripts/tests/test-install-debug.ps1), and validate
an isolated product install. Stage **both** `JoyeerRuntime` and `PRODUCT`; test
joypm with the staged compiler path rather than accidentally using the build-tree
compiler. Verify optional tool behavior and Windows Debug PDBs. Do not install
to the user's normal bin directory or update PATH merely to run acceptance.
Third-party notices remain a separate release requirement.

For each receiving-machine run, return:

- Commit SHA; OS and native hardware architecture; process and MSVC target
  architecture; preset; SDK/compiler/CMake/Ninja versions and LLVM SDK root.
- Configure/build results, CTest pass/fail totals, and all failed test names.
- The fresh CTest output log and, on failure, the build tree's
  `Testing/Temporary/LastTest.log` and `LastTestsFailed.log` when present.
- Failing stdout/stderr, generated manifest/argv/artifact paths, and any
  architecture, stream, Unicode, allocation-balance, or runtime-panic findings.
- Package/installer outcome and which remaining gates were **not** executed.

Keep fixture output directories on failure. Existing generations and partial
outputs are intentionally retained; do not hide a failure with a stale executable,
weaken assertions, or substitute a filtered test pass for the full gate.

## 6. Still open after a first green run

The [active release gates](package-manager.md#8-validation-and-release-gates)
remain authoritative. In particular, add/audit coverage for concurrent output
claims, physical aliases and link/device/FIFO inputs, full child/signal statuses,
host failures and partial initialization; validate debug-sidecar regeneration,
Release bootstrap and other native platforms. Add a joypm self-build case:
current bootstrap calls the compiler directly. A first green machine is a
recorded acceptance result, not a release announcement or universal support claim.