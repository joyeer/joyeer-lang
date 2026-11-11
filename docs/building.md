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
| macOS | Xcode Command Line Tools, a compatible macOS SDK, and matching LLVM/LLD development packages |

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
not stage the runtime needed to link programs. Unit tests, fern, and
Compression are enabled by default. Keep them enabled for acceptance;
filtered CTest runs are only for focused iteration.

### Full platform validation

For feature or platform acceptance, use the shared validator:

```text
python3 scripts/validate.py --llvm-root <sdk>
```

Use `python` instead of `python3` on Windows. The validator builds and runs
unfiltered CTest and the root workspace's `fern test` in **Debug and Release**.
Keep `BUILD_TESTING`, `JOYEER_BUILD_UNITTESTS`, `JOYEER_BUILD_FERN`, and
`JOYEER_BUILD_COMPRESSION` enabled. `--no-compression` covers only library
modules, not the full workspace.

Use `--report FILE` to save results, `--no-retry` to disable failed-test
reruns, and `--help` for other options. Validate each native architecture
separately; one platform's results do not establish another's acceptance.

### Fern bootstrap and acceptance

The default build bootstraps fern with the new compiler; no installed fern
is needed. Debug uses `-O0 -gfull`, Release uses `-O2 -g0`. On macOS, Debug
falls back to `-g0` if `dsymutil` is unavailable. Debug PDB/dSYM sidecars,
when produced, are tracked build outputs.

The [fern gates](../tests/fern/CMakeLists.txt) cover bootstrap regeneration,
self-build, workspaces, and relocated installs. They require `BUILD_TESTING`
and `JOYEER_BUILD_FERN`, independently of the C++ unit-test option.
`-DJOYEER_BUILD_FERN=OFF` disables bootstrap and these gates.

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
It selects the native Debug preset, installs into `~/.joyeer/bin`, updates
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

**Local staging is not a redistributable release.** Third-party license/notice
assembly is still required before redistribution.

Use your platform's Release preset, with `LLVM_HOME` set:

```text
cmake --preset <release-preset> -DINSTALL_GTEST=OFF
cmake --build --preset <release-preset>
ctest --preset <release-preset>
cmake --install out/build/<release-preset> --prefix out/package/joyeer
```

`INSTALL_GTEST=OFF` excludes GoogleTest from installation, not testing.
Keep LLVM SDKs and test tools out of product packages.

- `JoyeerRuntime` installs the compiler, backend, runtime, and Joyeer license.
- `PRODUCT` and `Moss` install fern and moss respectively when enabled.
- `StandardLibrary` installs standard modules, including Compression when enabled.
- `NativeDevelopment` provides optional C adapter headers and must be selected
  explicitly.

Keep the compiler, backend, and runtime together, with matching architectures.
Before redistribution, assemble applicable LLVM/LLD, support-library,
Windows LibXml2, Linux Clang, and other dependency notices.

**Homebrew macOS limitation:** the backend depends on shared LLVM/LLD, Z3,
and zstd libraries that the install does not bundle or relocate. A standalone
package needs a suitable static SDK or dependency bundling/relocation.
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
