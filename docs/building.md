# Building Joyeer

Run these commands from the repository root. For language usage, see the
[README](../README.md); for compiler internals, see [Backend](impl/backend.md).

## Source-build prerequisites

Install CMake 3.20+, Ninja, Git, a C++20 host compiler, and an **LLVM 22.1.8
development SDK** matching the native architecture.

| Platform | Additional requirements |
|---|---|
| Windows | Visual Studio Desktop development with C++, MSVC, Windows SDK, and DIA SDK |
| Linux | libc development files and startup objects; LLVM, LLD, and Clang development libraries from the same SDK |
| macOS ARM64 | Xcode Command Line Tools, a compatible macOS SDK, and matching LLVM/LLD development packages |

Intel and universal macOS builds are not supported. The `System` file bindings use
the native ARM64 filesystem ABI.

On Windows, use the full `clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz`
or `clang+llvm-22.1.8-aarch64-pc-windows-msvc.tar.xz` archive, not the
tool-only installer. LLVM/LLD libraries must use the static MSVC runtime
(`/MT`), and the DIA SDK must include libraries for the target architecture.
Build from an x64- or ARM64-targeting Visual Studio Developer shell.
The preset does not switch the shell's target; non-MSVC compilers are rejected.

Set `LLVM_HOME` to the SDK root, or pass `-DJOYEER_LLVM_ROOT=<sdk>` when
configuring. macOS also discovers Homebrew LLVM/LLD packages automatically.
See [troubleshooting](#troubleshooting) for SDK compatibility and discovery.

### Dependency policy

Toolchains are developer-managed: CMake does not install compilers, platform
SDKs, or LLVM. It acquires pinned project dependencies as needed:
LibXml2 2.14.5 on Windows, GoogleTest 1.15.2 for unit tests, and miniz 3.1.0
for the optional [Compression](impl/compression.md) module. First-time
dependency population requires network access.

Python is not required for source builds or compiled programs. The optional
installer and full-platform validator below require Python 3.9+.

## Configure, build, and test

Choose a native preset:

| Host | Debug | Release |
|---|---|---|
| Windows x64 | `x64-debug` | `x64-release` |
| Windows ARM64 | `arm64-debug` | `arm64-release` |
| Linux x86-64 / AArch64 | `linux-debug` | `linux-release` |
| macOS | `macos-debug` | `macos-release` |

For example, with `LLVM_HOME` set, Windows x64 uses:

```text
cmake --preset x64-debug
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Replace `x64-debug` in all three commands with your preset. Builds go under
`out/build/<preset>/`, with executables in `bin/`. Keep separate build trees
for each architecture; in-source builds are not supported.

Use the default build, not just `--target joyeer`: the compiler alone does
not stage the runtime archive under `bin/lib/`, where the compiler looks for
it when linking programs. Unit tests, fern, and Compression are enabled by
default. Keep them enabled for acceptance;
filtered CTest runs are only for focused iteration.

Linux runtime tests use the SDK's `llvm-nm`; on AArch64 they also use
`llvm-objdump` to distinguish C startup imports from program references.

For generated Linux programs, the raw `System` syscall path requires Linux 4.11
or newer for [`statx`](https://man7.org/linux/man-pages/man2/statx.2.html#HISTORY).
This is a syscall floor, not validation of a distribution running that kernel.
There is no glibc `statx` wrapper dependency. The minimum glibc for a linked
program follows the startup objects and libraries in its link environment.
For example, the Ubuntu 26.04 ARM64 link imports `__libc_start_main@GLIBC_2.34`;
[AArch64 programs linked and run against glibc 2.31](decisions/0016-thread-primitives.md#gating-and-the-link)
use the matching older startup and, when needed, `libpthread.so.0`. This is not
a glibc compatibility promise for the compiler or third-party native libraries.

The Debug presets also set `JOYEER_BACKEND_TEST_HOOKS=ON`: the backend verifies
the optimized module again and accepts a test fault injection. It is off by
default and in the Release presets, which build the packages; never enable it
for a package. A local Debug installation carries it, as a developer build.
Configuring without a preset leaves it off, so add `-DJOYEER_BACKEND_TEST_HOOKS=ON`
to run `E2EOptimizedDebugInfoIsVerified`.

### Full platform validation

For feature or platform acceptance, use the shared validator:

```text
python3 scripts/validate.py --llvm-root <sdk>
```

Use `python` instead of `python3` on Windows. The validator builds and runs
unfiltered CTest and the root workspace's `fern test` in **Debug and Release**.
Keep `BUILD_TESTING`, `JOYEER_BUILD_UNITTESTS`, `JOYEER_BUILD_FERN`, and
`JOYEER_BUILD_COMPRESSION` enabled; the validator also enables
`JOYEER_BACKEND_TEST_HOOKS`. `--no-compression` covers only library
modules, not the full workspace.

Use `--report FILE` to save results, `--no-retry` to disable failed-test
reruns, and `--help` for other options. Validate each native architecture
separately; one platform's results do not establish another's acceptance.

For new language regressions, cite the most specific applicable spec section
with a `// spec: 04-memory.md §4.2` comment. Run `python3 scripts/spec-coverage.py`
(`python` on Windows) to list numbered sections with no explicit citations.
The report rejects invalid citations; a citation records traceability, not
proof that every case in a section is covered.

For local sanitizer checks, configure a separate Debug build tree with
`-DJOYEER_SANITIZE_HOST=ON`. This instruments the C++ compiler, backend, and
C++ test drivers with MSVC AddressSanitizer or GCC/Clang ASan and UBSan.
It does not instrument the Joyeer-emitted runtime archive, generated
programs, or dependencies already built into the LLVM SDK. Use the existing
host toolchain's sanitizer runtime; do not redistribute these builds.

On native Linux or macOS ARM64, add `-DJOYEER_SANITIZE_RUNTIME=ON` in a
separate build with `BUILD_TESTING` and `JOYEER_BUILD_UNITTESTS` enabled and
the SDK's `llvm-nm` available. Linux supports GCC and Clang. On macOS, use
the LLVM SDK's Clang and matching `compiler-rt` for the C probe
(`-DCMAKE_C_COMPILER=<llvm-root>/bin/clang` in a fresh build tree).
AppleClang's ASan runtime is not compatible with this instrumentation.

```text
cmake --build <sanitizer-build> --target RuntimeAsanProbe
ctest --test-dir <sanitizer-build> -R '^RuntimeAddressSanitizer[.]' --no-tests=error --output-on-failure
```

The probe links a test-only archive. It checks valid reads and formatting
and requires ASan reports for a one-byte overread and a two-byte formatting
write into a one-byte buffer.
Only the test build's explicit `--emit-asan-runtime-object` operation
enables instrumentation. The object must have a core root definition and
may not contain non-core definitions from dependencies. Ordinary object
and executable emission are unchanged. The probe uses C heap buffers;
ASan does not automatically track logical bounds within the runtime's
custom allocator. Instrumenting arbitrary generated programs and Windows
runtime objects is outside this test suite. The host and runtime options
are independent and can be enabled together.

### Fern bootstrap and acceptance

The default build bootstraps fern with the new compiler; no installed fern
is needed. Debug uses `-O0 -gfull`, Release uses `-O2 -g0`. On macOS, Debug
falls back to `-g0` if `dsymutil` is unavailable. Debug PDB/dSYM sidecars,
when produced, are tracked build outputs.

Subsequent macOS native Debug builds run by fern pass `-gauto`, so they
follow the selected compiler's `dsymutil` configuration. Explicit `-gfull`
requests still require `dsymutil` for native executable output.
To verify a macOS configuration without it, pass
`--define JOYEER_DSYMUTIL_EXECUTABLE=` to `scripts/validate.py` in a separate
validation checkout. Tests that require a dSYM are not registered there;
the explicit missing-tool diagnostic and ordinary native workflows still run.

For the native-link and Fern acceptance handoff on a Mac ARM64, run from
the checkout of the commit being validated:

```bash
python3 scripts/validate-macos.py --llvm-root "$(brew --prefix llvm)" --sdk macosx26.5
```

Use your installed LLVM 22.1.8 SDK path if it is not managed by Homebrew;
`--lld-dir` can select an existing separate LLD CMake package. Use
`--sdk macosx` if the active SDK is compatible. The script runs the focused
native-link tests, the full Debug and Release validator, the focused Fern
tests, and a separate Debug build with dSYM generation disabled. It prints
the directory with the two isolated checkouts, reports, and provenance.
It validates committed `HEAD` without changing the source checkout or
marking plans complete.

The [fern gates](../tests/fern/CMakeLists.txt) cover bootstrap regeneration,
self-build, workspaces, and relocated installs. They require `BUILD_TESTING`
and `JOYEER_BUILD_FERN`, independently of the C++ unit-test option.
`-DJOYEER_BUILD_FERN=OFF` disables bootstrap and these gates.

### Toolchain library layout

The compiler and fern sit beside `lib/`, which contains `JoyeerRuntime.lib` or
`libJoyeerRuntime.a`, `index.toml`, and the source modules `Foundation`, `System`,
and optional `Compression`. Development builds stage this layout under `bin/`;
the `JoyeerRuntime` and `StandardLibrary` install components install the
archive and the modules/index separately. Fern reads the index beside the
compiler it selects. See [fern discovery](impl/fern.md#2-commands-and-selection)
and the [compiler CLI](impl/backend.md#2-cli) for their respective overrides.

On 2026-11-28 at `a362c88`, Windows ARM64 and Linux AArch64 (Ubuntu 26.04.1
under WSL2), both with LLVM/LLD 22.1.8, passed unfiltered `scripts/validate.py`
in Debug and Release without retries. Per configuration, Windows passed
2260/2260 CTests and Linux passed 2235/2235; both passed all 7 root `fern test`
targets. Each run included `Fern.Installed` (component inventories and relocated
fern workflows) and `InstalledCompilerNativeHello`. See the
[x86-64 results](decisions/0031-equatable.md#platform-validation) and
[macOS ARM64 relocation results](impl/fern.md#macos-arm64-toolchain-validation).
These layout gates do not establish standalone distributable packaging.

### Native validation workflow

Local native runs of [`scripts/validate.py`](#full-platform-validation)
satisfy the validation gates; CI runners are not required. The optional
[Native validation workflow](../.github/workflows/validate.yml) supports
weekly and manual runs on the default branch. To opt in, set the repository
variable `JOYEER_NATIVE_RUNNERS` to `enabled` after provisioning self-hosted
runners with the `joyeer-native` label, the native toolchains above,
`LLVM_HOME`, and Python 3.9+. On Windows set `JOYEER_VCVARSALL` to the installed
`vcvarsall.bat`. The workflow does not acquire compilers or SDKs or run
untrusted pull request code on self-hosted runners.

## Local Debug installation

After building a native Debug preset:

```powershell
python .\scripts\install-debug.py
```

On macOS or Linux:

```bash
python3 scripts/install-debug.py
```

The [installer](../scripts/install-debug.py) requires Python 3.9+ and CMake.
It selects the native Debug preset, installs the toolchain and its bundled
standard modules into `~/.joyeer/bin`, updates
the user PATH or shell profile, and installs the [Joyeer agent skill](../skills/README.md).
**Open a new terminal or restart your IDE afterward.**

- Use `--build-dir` for a different existing Debug build and `--install-dir`
  for a different binary destination.
- Use `--skip-path-update` to leave PATH and shell profiles untouched; it is
  required for unsupported shells.
- Use `--skip-skill-install` to omit the skill or `--skill-dir` to choose
  its destination. See [skill installation](../skills/README.md#install-for-a-local-agent)
  before using `--force` to overwrite local skill edits.

The installer does not configure or build Joyeer, and does not fall back to
another architecture or Release build. Close running tools/debuggers before
reinstalling; rerun the same command after rebuilding.

Native compilation still needs the platform SDK and system libraries.
Self-contained backends do not require an external LLVM installation;
Homebrew-backed macOS installs retain the dependencies described below.

## Release staging

**Local staging is not a redistributable release.** Audit native dependencies
and their actual redistribution terms before distributing a package.

Use your platform's Release preset, with `LLVM_HOME` set:

```text
cmake --preset <release-preset> -DINSTALL_GTEST=OFF -DJOYEER_BACKEND_TEST_HOOKS=OFF -DJOYEER_SANITIZE_HOST=OFF -DJOYEER_SANITIZE_RUNTIME=OFF
cmake --build --preset <release-preset>
ctest --preset <release-preset>
python3 scripts/stage_release.py --build-dir out/build/<release-preset> --notice-dir <reviewed-notices> --output-dir <new-stage>
```

`INSTALL_GTEST=OFF` excludes GoogleTest from installation, not testing.
Use `python` on Windows. The stage must not exist, and its parent must
already exist. The script installs only the selected product components,
not the SDK or test tools; it refuses test-hook and sanitizer builds.

- `JoyeerRuntime` installs the compiler and backend at the prefix, the static
  runtime archive under `<prefix>/lib/`, and the Joyeer license.
- `PRODUCT` and `Moss` install fern and moss respectively when enabled.
- `StandardLibrary` installs standard modules, including Compression when enabled.
- `NativeDevelopment` provides optional C adapter headers and must be selected
  explicitly.

Keep the compiler, backend, and runtime in this layout, with matching architectures.
The concurrency, enum-role and collections preludes (`lib/Runtime/{concurrency,roles,collections}.joyeer`)
are embedded in that compiler and aren't installed as separate source files.

Collect the exact notice files from the build's SDKs and dependencies in
`--notice-dir`: `LLVM.txt`, `LLD.txt`, `Platform.txt`, plus `LibXml2.txt`
on Windows or `Clang.txt` on Linux. `Platform.txt` records the SDK and
support-library audit; add any other required notices as separate `.txt`
files. The script copies them verbatim and writes a sorted relative-path
inventory with each file's size and SHA-256. It does not infer a license
from a binary, supply missing legal text, or certify redistribution.
Compression already carries the miniz notice in its module package.
An interrupted or failed stage is not a publishable package; use a new
destination on retry.

`Fern.Installed` verifies separate components, the combined inventory
(including Moss and Compression when enabled), and relocation to a path
with spaces and Unicode. Inspect the staged dependencies separately with
`llvm-readobj --coff-imports` on Windows, `readelf -d` on Linux, or
`otool -L` on macOS, and verify any transitive SDK dependencies on the
intended deployment host.

On Linux AArch64 under WSL2, an LLVM/LLD 22.1.8 production Release build
staged 42 files and ran the relocated compiler, fern, and Moss. The
backend still needed the SDK's shared `libLLVM.so.22.1`, zlib, and zstd
in addition to the system runtime; this is relocation evidence on the
build host, not a standalone distribution.

**Shared SDK limitation:** the backend follows the SDK's LLVM linkage on
Linux and macOS. With a shared SDK (such as LLVM's Ubuntu packages or
Homebrew), the install does not bundle or relocate the backend's shared
dependencies. Even for a local Debug install, the matching libraries must be
on the system loader's search path (`LLVM_HOME` only selects the build SDK).
A standalone package needs a suitable static SDK or dependency bundling/relocation.
Generated Joyeer programs do not inherit LLVM/LLD dependencies.

## Troubleshooting

- **CMake fails after moving a checkout:** configure a new build tree; cached
  source paths refer to the old location.
- **LLVM/LLD packages are missing:** use development libraries, not a tool-only
  installer. Set `JOYEER_LLVM_ROOT`, or `LLVM_DIR`/`LLD_DIR`/`Clang_DIR` for a
  nonstandard layout. Linux needs `clangDriver` as well as `lldELF`.
- **Windows compiler or SDK errors:** use a matching Visual Studio Developer
  shell and verify `cl`, the C++ workload, Windows SDK, and LLVM architecture.
  Packaged compilers also need MSVC Build Tools and a Windows SDK to link.
- **Dependency fetch fails:** restore Git/network access or provide the
  dependency through CMake's normal dependency cache.
- **macOS TAPI errors involving `arm64e.x1`:** LLVM/LLD 22.1.8 cannot read the
  tested macOS 27.0/27.2 SDKs. Select an installed compatible SDK (26.5 was
  validated) using `CMAKE_OSX_SYSROOT`, `CMAKE_OSX_DEPLOYMENT_TARGET`,
  `JOYEER_MACOS_SDK_PATH`, `JOYEER_MACOS_SDK_VERSION`, and
  `JOYEER_MACOS_DEPLOYMENT_TARGET` consistently.
- **macOS debug output fails:** `-gfull` requires `dsymutil`; use `-g0` when
  it is unavailable. Check the active SDK with `xcrun --sdk macosx --show-sdk-path`.
- **Linux reports `invalid linker name ... '-fuse-ld=lld'`:** put the SDK's
  `bin` directory on PATH. The in-process Clang Driver checks for `ld.lld`
  even though it never launches it; the validator sets this PATH when needed.
- **Linux startup objects or libc are missing:** install the distribution's
  libc and GCC runtime development packages; embedded LLD does not replace them.
