# Building Joyeer

All supported platforms perform code generation and native linking through
LLVM/LLD libraries behind the versioned Joyeer native-backend C ABI. The
platform backend selects COFF, Mach-O, or ELF LLD in-process. Linux uses the
Clang Driver library in-process to discover startup objects, the dynamic
loader, libc, and compiler runtime inputs; it never launches Clang to produce
native output.

## Dependency policy

Contributors install and update CMake, Ninja, the host compiler, platform SDK,
and LLVM 22.1.8 development SDK. CMake does not download or build LLVM.
All platforms treat LLVM, LLD, and the Linux Clang Driver library as private
implementation details of a Joyeer-owned backend library. Only a versioned C
ABI crosses into the compiler; LLVM/Clang C++ types, exceptions, allocators,
and ownership never cross that boundary.

On Windows, CMake fetches and statically embeds LibXml2 2.14.5 because the
official LLVM 22.1.8 Windows LLD libraries enable manifest merging. On every
platform, `JOYEER_BUILD_UNITTESTS=ON` also fetches GoogleTest 1.15.2. The first
configuration that needs these dependencies requires network access. Neither
dependency adds a runtime DLL to the Joyeer package.

## Source-build prerequisites

All platforms require:

- CMake 3.20 or newer;
- Ninja as the canonical generator;
- a host C and C++ compiler with C++20 support;
- a developer-installed LLVM 22.1.8 development SDK;
- the platform SDK and native linker inputs;
- network access for the first LibXml2 and GoogleTest population.

Windows requires the full development archive matching the MSVC target,
not the tool-only installer:

```text
clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz
clang+llvm-22.1.8-aarch64-pc-windows-msvc.tar.xz
```

Use the first archive for x64 or the second for ARM64. The LLVM/LLD libraries
must use the static MSVC runtime (`/MT`). Install the matching MSVC target
tools, Windows SDK libraries, and Visual Studio DIA SDK (`lib/amd64` for x64,
`lib/arm64` for ARM64). CMake checks the LLVM native architecture against the
MSVC target; x64 LLVM libraries cannot be linked into an ARM64 backend DLL.

Extract the SDK to a stable path such as `C:\LLVM-22.1.8` for x64 or
`C:\LLVM-22.1.8-arm64` for ARM64, then set the user environment variable
`LLVM_HOME` to the selected SDK. For x64, in PowerShell:

```powershell
[Environment]::SetEnvironmentVariable("LLVM_HOME", "C:\LLVM-22.1.8", "User")
$env:LLVM_HOME = "C:\LLVM-22.1.8"
```

The first command persists the setting for future applications; the second
sets it in the current shell. Restart already-running IDEs or terminal hosts
so their new terminals inherit the updated user environment.

The directory must contain:

```text
bin/clang.exe
bin/llvm-readobj.exe
lib/LLVMCore.lib
lib/lldCOFF.lib
lib/cmake/llvm/LLVMConfig.cmake
lib/cmake/lld/LLDConfig.cmake
```

The LLVM executables are test and inspection tools. `joyeer` does not launch
Clang or an LLD executable on any platform. Python and curl are not
source-build requirements.

Platform prerequisites are:

| Platform | Required software |
|---|---|
| Windows | Visual Studio with Desktop development with C++, MSVC, a Windows SDK, CMake, and Ninja |
| Linux | GCC or Clang with C++20 support, libc development files and startup objects, the LLVM/LLD/Clang 22.1.8 development SDK, CMake, and Ninja |
| macOS | Xcode Command Line Tools and the active macOS SDK, CMake, Ninja, and Homebrew `llvm`/`lld` 22.1.8 |

Windows uses the MSVC compiler toolset installed by Visual Studio for the
Joyeer implementation. CMake rejects MinGW, GCC, clang-cl, and other non-MSVC
compilers. Ninja is the canonical build generator on Windows, but it only
schedules compiler and linker commands; it does not replace `cl.exe` or the
Visual Studio toolchain. Platform SDKs, sysroots, startup objects, and system
libraries remain external prerequisites.

Before configuring Windows, verify the tools in Developer PowerShell:

```powershell
cmake --version
ninja --version
git --version
& "$env:LLVM_HOME\bin\clang.exe" --version
& "$env:LLVM_HOME\bin\llvm-readobj.exe" --version
```

Run `cl` from a Visual Studio Developer shell as well. All LLVM tools and
libraries must report version 22.1.8 and come from the same SDK root.

On macOS, install the matching development libraries and verify the active
SDK. `dsymutil` is optional for ordinary native output and required only when
`-g` requests a dSYM bundle:

```text
brew install cmake ninja llvm lld
brew list --versions llvm lld
xcrun --sdk macosx --show-sdk-path
xcrun --sdk macosx --show-sdk-version
```

## Configure, build, and test

Run CMake from a shell in which the host compiler and platform SDK are active.
On macOS, the checked-in presets use Ninja, discover the LLVM/LLD CMake
packages, and obtain the active SDK through `xcrun`:

```text
cmake --preset macos-debug
cmake --build --preset macos-debug
ctest --preset macos-debug
```

Use `macos-release` in the same commands for a release build.

On Linux, use the full LLVM/LLD/Clang development SDK and the checked-in
presets:

```text
cmake --preset linux-debug -DJOYEER_LLVM_ROOT=/opt/llvm
cmake --build --preset linux-debug
ctest --preset linux-debug
```

Use `linux-release` for a release build. Omit `JOYEER_LLVM_ROOT` when
`LLVM_HOME` already points to the SDK.

Linux presets are architecture-neutral. Native x86-64 and AArch64 builds use
the same commands with a matching host compiler, LLVM/LLD/Clang SDK, and
system development libraries. Code generation uses LLVM's default target;
the Clang Driver discovers that target's ELF linker inputs. Run the full
acceptance gate on each Linux architecture before treating it as validated;
Windows ARM64 results do not establish Linux ARM64 coverage.

On Windows, use a Visual Studio Developer PowerShell or Developer Command
Prompt configured for the intended x64 or ARM64 target. For x64, use the x64
Native Tools prompt or `vcvars64.bat`; for native ARM64, use an ARM64-targeting
Developer shell, for example one initialized by `vcvarsarm64.bat`. The
presets select `cl.exe` but declare architecture setup as external: the
preset name does not switch the shell's compiler target. Configuration fails
when the required Visual Studio C++ workload or Windows SDK is missing.

All Windows Debug and Release presets enable `JOYEER_BUILD_UNITTESTS` by
default. For x64 development and the full acceptance gate:

```powershell
cmake --preset x64-debug -DJOYEER_LLVM_ROOT=C:\LLVM-22.1.8 -DJOYEER_BUILD_UNITTESTS=ON
cmake --build --preset x64-debug
ctest --preset x64-debug
```

For native Windows ARM64, from an ARM64-targeting Developer shell:

```powershell
cmake --preset arm64-debug -DJOYEER_LLVM_ROOT=C:\LLVM-22.1.8-arm64
cmake --build --preset arm64-debug
ctest --preset arm64-debug
```

Use `arm64-release` for a release build. Keep separate build directories
and SDK roots for x64 and ARM64;
do not switch architectures inside an existing CMake build tree.

These are native builds, not a cross-compilation interface. The compiler,
backend library, native runtime archive, LLVM default target, and system
libraries must agree on the architecture. Joyeer does not currently expose
a `--target` option; 32-bit Windows and ARM64EC are not supported.

Omit `JOYEER_LLVM_ROOT` when `LLVM_HOME` points to the intended SDK.

The build is out-of-source only. Preset builds write the executable under
`out/build/<preset>/bin/`; manually configured single-config builds use
`build/bin/`. Unfiltered CTest is the required final CMake gate; labels are for
focused iteration.

## Release staging

Build Release and stage a movable Windows package directory:

```powershell
cmake --preset x64-release -DJOYEER_LLVM_ROOT=C:\LLVM-22.1.8 -DJOYEER_BUILD_UNITTESTS=ON -DINSTALL_GTEST=OFF
cmake --build --preset x64-release
ctest --test-dir .\out\build\x64-release --output-on-failure
cmake --install .\out\build\x64-release --prefix .\out\package\joyeer
```

For an ARM64 package, use `arm64-release` and the ARM64 LLVM SDK throughout
the commands above, with a separate staging directory. Do not mix x64 and
ARM64 compiler, backend, or runtime files in one package.

`INSTALL_GTEST=OFF` disables only GoogleTest's installation rules, not its
tests. Without this setting, a unit-test-enabled build also installs
`include/gtest`, GoogleTest libraries, and CMake/pkg-config metadata.

With that setting, the Joyeer package contains:

```text
joyeer.exe
joyeer-backend.dll
JoyeerNativeRuntime.lib
licenses/
```

The current top-level install rule places only the repository's own `LICENSE`
in `licenses/`. This is not yet a complete redistribution-license bundle:
LLVM/LLD and, on Windows, LibXml2 license/notice material must also be staged
as required by those dependencies. Audit the final manifest and notices before
publishing a release; a successful native smoke run alone does not establish
release readiness.

These files may be moved together to another directory or machine. LLVM,
LLD, and LibXml2 are statically contained in the backend DLL. The DLL finds
installed MSVC and Windows SDK library paths through Visual Studio Setup
Configuration and the Windows registry, so a Developer Prompt is not required
when running a packaged compiler.

Release users do not install LLVM or Clang. They still need MSVC Build Tools
and a Windows SDK to create native executables. Programs already produced by
Joyeer use the static CRT and Windows system DLLs; running those programs does
not require the Visual C++ Redistributable.

Linux and macOS use the corresponding release preset:

```text
cmake --preset linux-release -DJOYEER_LLVM_ROOT=/opt/llvm -DINSTALL_GTEST=OFF
cmake --build --preset linux-release
cmake --install out/build/linux-release --prefix out/package/joyeer
```

Replace `linux-release` with `macos-release` on macOS. The installed package
keeps the `joyeer` executable, `joyeer-backend` shared library,
`JoyeerNativeRuntime` archive, and licenses together. Linux uses an `$ORIGIN`
runtime search path and macOS uses `@loader_path`; the compiler also resolves
the runtime archive relative to its own executable. Building Joyeer programs
still requires the host libc/platform SDK startup objects and system
libraries, but not an external LLVM, Clang, or LLD executable.

## Local Windows Debug installation

[scripts/install-debug.ps1](../scripts/install-debug.ps1) installs an **existing
Debug build** for local development and testing. It requires Windows PowerShell
5.1 or PowerShell 7 and CMake on PATH, but not Python. It does not configure or
build Joyeer, download tools, or create a ZIP. It also installs the matching
Joyeer agent skill from this checkout by default.

Configure and build in a matching Developer shell first. For ARM64:

```powershell
cmake --preset arm64-debug -DJOYEER_LLVM_ROOT=C:\LLVM-22.1.8-arm64
cmake --build --preset arm64-debug
.\scripts\install-debug.ps1
```

For x64, build `x64-debug` with the x64 LLVM SDK. With no `-BuildDir`, the installer
selects `out/build/arm64-debug` on ARM64 Windows or `out/build/x64-debug` on x64
Windows, relative to this checkout. It reads the Windows system architecture,
not the shell process or Developer shell target, so an emulated shell does not
change the default. The matching Debug build must already exist; the script
does not configure, build, or fall back to another architecture. Use `-BuildDir`
to select a different existing Debug build explicitly. If the system
architecture cannot be identified, specify `-BuildDir` instead of relying on
potentially emulated process-architecture environment variables.

Installation can run from ordinary PowerShell after the build. The default destination is
`$HOME\.joyeer\bin` (`~/.joyeer/bin`), which needs no administrator rights.
Use `-InstallDir` for a dedicated alternative destination or to keep separate
architecture installations:

```powershell
.\scripts\install-debug.ps1 -BuildDir .\out\build\arm64-debug -InstallDir C:\DevTools\Joyeer-debug-arm64
```

After successful installation, the script adds the install directory to the
user PATH unless the user or system PATH already contains it. It also updates
the current PowerShell PATH, so `joyeer` is immediately available. Comparison
ignores case and trailing directory separators and expands environment
variables; existing PATH entries and their order are preserved. System PATH
is never modified. Restart other open terminals or IDEs to inherit user PATH
changes. Pass `-SkipPathUpdate` to leave both persistent and current PATH
unchanged, for example for temporary installations.

The complete `skills/joyeer` directory, including references and examples, is
copied to `$HOME\.agents\skills\joyeer` for VS Code Copilot, Copilot CLI, and
Codex. Use `-SkillDir` to specify the full destination directory for another
client or scope, for example for Claude Code:

```powershell
.\scripts\install-debug.ps1 -SkillDir "$HOME\.claude\skills\joyeer"
```

Only the selected skill destination is used. Pass `-SkipSkillInstall` for a
compiler-only installation. For isolated installations, override both
`-InstallDir` and `-SkillDir` (or skip the skill), as well as `-SkipPathUpdate`.
If the skill already contains identical source files, it is left unchanged,
including any additional user files. A new or empty destination accepts a full
installation. In a nonempty destination, missing or differing files cause an
error before compiler installation or PATH updates unless `-Force` is supplied.
After reviewing and backing up any local skill edits, update with:

```powershell
.\scripts\install-debug.ps1 -Force
```

`-Force` applies only to skill files: it overwrites differing files and restores
missing ones using the current checkout. Identical files, extra files
(including files removed from the source in a newer revision), and other skills
are left unchanged. It does not clear the destination or bypass Debug build
validation. File/directory type conflicts are rejected even with `-Force`;
resolve those conflicts manually. `-SkipSkillInstall` takes precedence over
`-Force`. Reload the agent session after installation and confirm discovery;
see [agent setup](../skills/README.md).

The script verifies that the build belongs to this checkout and selects Debug
only, including in multi-configuration build trees. All runtime binaries and
compiler/backend PDBs must already exist. Installation uses the `JoyeerRuntime`
CMake component to exclude GoogleTest and SDK files, then copies the PDBs:

```text
joyeer.exe
joyeer-backend.dll
JoyeerNativeRuntime.lib
joyeer.pdb
joyeer-backend.pdb
licenses\LICENSE
```

After rebuilding, rerun the same command to update the installation. Close
running compiler processes or debugger sessions first to release file locks.
Unrelated files in the destination are not removed. This is a local Debug
installation, not a distributable release; third-party license/notice auditing
is still required for redistribution.

Use the installed compiler directly from the repository root (adjust the path
when using `-InstallDir`):

```powershell
& "$HOME\.joyeer\bin\joyeer.exe" -O0 -gfull -gcodeview -o .\out\hello.exe .\tests\native\hello.joyeer
if ($LASTEXITCODE -ne 0) { throw "Compilation failed" }
.\out\hello.exe
if ($LASTEXITCODE -ne 0) { throw "Program failed" }
```

MSVC Build Tools and a matching Windows SDK are still required for native
compilation, but LLVM is not needed to use the installed compiler. Compiler
PDBs support debugging Joyeer itself; `-gfull -gcodeview` generates a separate
PDB for the compiled Joyeer program.

Focused installer tests use an isolated temporary directory and an existing
Debug build; they do not modify your user installation, agent skills, or PATH. They also test
automatic selection, which requires the native architecture's default Debug
build to be present:

```powershell
.\scripts\tests\test-install-debug.ps1 -BuildDir .\out\build\arm64-debug
```

## Non-Windows tool discovery

macOS requires LLVM and LLD 22.1.8 development packages. CMake discovers the
Homebrew packages under `/opt/homebrew/opt` or `/usr/local/opt`; set
`JOYEER_LLVM_ROOT`, `LLVM_DIR`, or `LLD_DIR` for another SDK layout. It obtains
the active SDK path and version from `xcrun`; set `JOYEER_MACOS_SDK_PATH` and
`JOYEER_MACOS_SDK_VERSION` together when an explicit SDK is necessary. Set
`JOYEER_MACOS_DEPLOYMENT_TARGET` to target an older supported macOS release;
it defaults to `CMAKE_OSX_DEPLOYMENT_TARGET`, then to the active SDK version.

Linux requires LLVM, LLD, and Clang **development libraries** from the same
22.1.8 SDK. The root must provide `LLVMConfig.cmake`, `LLDConfig.cmake`,
`ClangConfig.cmake`, `lldELF`, and `clangDriver`. The Clang Driver library
constructs the host link command in-process and embedded ELF LLD executes it.
The host libc development package must provide its startup objects and system
libraries in a layout recognized by Clang.

`JOYEER_CLANG_EXECUTABLE` refers only to the optional Clang executable used by
independent LLVM IR oracle tests. It is never launched by `joyeer` when
building a native executable.

## Troubleshooting

- **LLVM or LLD package is not found:** use the full Windows development
  archive, a full Linux LLVM/LLD/Clang development SDK, or install both `llvm`
  and `lld` with Homebrew on macOS; clear the build directory or use
  `cmake --fresh`, and set `JOYEER_LLVM_ROOT`, `LLVM_DIR`, `LLD_DIR`, or
  `Clang_DIR` for a nonstandard SDK root.
- **Windows rejects the compiler:** reopen a Visual Studio Developer shell and
  verify `cl` and the Windows SDK before running CMake.
- **An existing Windows source path is reported missing:** the current CLI
  receives narrow `argv` strings. Characters outside the system ANSI code page
  can be lost before filesystem access, even when the file exists. Use paths
  representable in that code page until wide-character argument handling is
  implemented; changing the source file's text encoding does not fix this.
- **Native compilation crashes with a Unicode temporary directory:** a
  separate path-construction bug narrows `TEMP`/`TMP`-derived paths before the
  backend call. An ASCII temporary directory avoids that current defect; the
  implementation must preserve native paths and report conversion failures.
- **Packaged native linking cannot find platform libraries:** install the
  Desktop development with C++ workload and a Windows SDK.
- **GoogleTest cannot be fetched:** restore Git/network access, provide it
  through the normal CMake dependency cache, or configure with
  `-DJOYEER_BUILD_UNITTESTS=OFF`.
- **macOS native linking cannot find system libraries:** run `xcode-select -p`
  and `xcrun --sdk macosx --show-sdk-path`, then repair or select Xcode Command
  Line Tools before reconfiguring.
- **Linux native linking cannot find startup objects or libc:** install the
  distribution's libc and GCC runtime development packages; embedded LLD does
  not replace those platform ABI inputs.
