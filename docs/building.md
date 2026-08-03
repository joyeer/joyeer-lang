# Building Joyeer

Windows and macOS perform code generation and native linking through
LLVM/LLD libraries behind the versioned Joyeer native-backend C ABI. Windows
statically packages those libraries in `joyeer-backend.dll`; macOS
links the Homebrew LLVM and LLD dynamic libraries. Linux continues to invoke
an external Clang driver until its ELF backend library is implemented.

## Dependency policy

Contributors install and update CMake, Ninja, the host compiler, platform SDK,
and LLVM 22.1.8 development SDK. CMake does not download or build LLVM. On
Windows and macOS treat LLVM/LLD libraries as private implementation details
of a Joyeer-owned backend library. Only a versioned C ABI crosses into the
compiler; LLVM C++ types, exceptions, allocators, and ownership never cross
that boundary.

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

The LLVM executables are test and inspection tools. Windows `joyeer.exe` does
not launch them. Python and curl are not source-build requirements.

Platform prerequisites are:

| Platform | Required software |
|---|---|
| Windows | Visual Studio with Desktop development with C++, MSVC, a Windows SDK, CMake, and Ninja |
| Linux | GCC or Clang with C++20 support, libc development files, binutils, CMake, and Ninja |
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
SDK:

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

## Windows release staging

Build Release and install a movable package directory:

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

## Non-Windows tool discovery

macOS requires LLVM and LLD 22.1.8 development packages. CMake discovers the
Homebrew packages under `/opt/homebrew/opt` or `/usr/local/opt`; set
`JOYEER_LLVM_ROOT`, `LLVM_DIR`, or `LLD_DIR` for another SDK layout. It obtains
the active SDK path and version from `xcrun`; set `JOYEER_MACOS_SDK_PATH` and
`JOYEER_MACOS_SDK_VERSION` together when an explicit SDK is necessary. Set
`JOYEER_MACOS_DEPLOYMENT_TARGET` to target an older supported macOS release;
it defaults to `CMAKE_OSX_DEPLOYMENT_TARGET`, then to the active SDK version.

Linux still requires an external Clang 22.1.8 driver. Set
`JOYEER_CLANG_EXECUTABLE` when it is not discoverable through `PATH`. Clang is
also discovered on Windows and macOS for IR oracle tests, but the compiler does
not launch it to produce native executables on those platforms.

## Troubleshooting

- **LLVM or LLD package is not found:** use the full Windows development
  archive or install both `llvm` and `lld` with Homebrew on macOS, clear the
  build directory or use `cmake --fresh`, and set `JOYEER_LLVM_ROOT`,
  `LLVM_DIR`, or `LLD_DIR` for a nonstandard SDK root.
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
