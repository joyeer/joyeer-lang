# Building Joyeer

All supported platforms perform code generation and native linking through
LLVM/LLD libraries behind the versioned Joyeer native-backend C ABI. The
platform backend selects COFF, Mach-O, or ELF LLD in-process. Linux uses the
Clang Driver library in-process to discover startup objects, the dynamic
loader, libc, and compiler runtime inputs; it never launches Clang to produce
native output.

## Dependency policy

Contributors install and update CMake, Ninja, the host compiler, platform SDK,
and LLVM 22.1.8 development SDK. CMake does not download or build LLVM. On
All platforms treat LLVM, LLD, and the Linux Clang Driver library as private
implementation details of a Joyeer-owned backend library. Only a versioned C
ABI crosses into the compiler; LLVM/Clang C++ types, exceptions, allocators,
and ownership never cross that boundary.

CMake fetches and statically embeds LibXml2 2.14.5 because the official LLVM
22.1.8 Windows LLD libraries enable manifest merging. When
`JOYEER_BUILD_UNITTESTS=ON`, CMake also fetches GoogleTest 1.15.2. The first
configuration therefore requires network access. Neither dependency adds a
runtime DLL to the Joyeer package.

## Source-build prerequisites

All platforms require:

- CMake 3.20 or newer;
- Ninja as the canonical generator;
- a host C and C++ compiler with C++20 support;
- a developer-installed LLVM 22.1.8 development SDK;
- the platform SDK and native linker inputs;
- network access for the first LibXml2 and GoogleTest population.

Windows requires the full development archive, not the tool-only installer:

```text
clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz
```

Extract it to a stable path such as `D:\llvm`, then set the user environment
variable `LLVM_HOME=D:\llvm`. The directory must contain:

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

Before configuring Windows, verify the tools supplied by your environment:

```text
cmake --version
ninja --version
git --version
%LLVM_HOME%\bin\clang.exe --version
%LLVM_HOME%\bin\llvm-readobj.exe --version
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

On Windows, use a Visual Studio Developer PowerShell or Developer Command
Prompt. The Windows presets explicitly select `cl.exe`; configuration fails
when the Visual Studio C++ workload or Windows SDK is missing.

Configure, build, and test with:

```text
cmake --preset x64-debug -DJOYEER_LLVM_ROOT=D:/llvm -DJOYEER_BUILD_UNITTESTS=ON
cmake --build --preset x64-debug
ctest --preset x64-debug
```

Omit `JOYEER_LLVM_ROOT` when `LLVM_HOME` points to the intended SDK.

The build is out-of-source only. Preset builds write the executable under
`out/build/<preset>/bin/`; manually configured single-config builds use
`build/bin/`. Unfiltered CTest is the required final CMake gate; labels are for
focused iteration.

## Release staging

Build Release and install a movable Windows package directory:

```text
cmake --preset x64-release -DJOYEER_LLVM_ROOT=D:/llvm
cmake --build --preset x64-release
cmake --install out/build/x64-release --prefix out/package/joyeer
```

The package contains:

```text
joyeer.exe
joyeer-backend.dll
JoyeerNativeRuntime.lib
licenses/
```

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
cmake --preset linux-release -DJOYEER_LLVM_ROOT=/opt/llvm
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
