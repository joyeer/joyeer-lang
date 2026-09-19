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
