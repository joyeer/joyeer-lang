# Building Joyeer

Joyeer uses the host platform toolchain plus an external LLVM installation.
The repository does not download, build, package, or select those tools.

## Dependency policy

Contributors install and update CMake, Ninja, the host compiler, the platform
SDK, and Clang/LLVM. CMake discovers and uses them but does not install them.
LLVM is not a Git submodule, a CMake `FetchContent` dependency, or a project
superbuild. Joyeer deliberately emits textual LLVM IR and invokes the external
Clang driver instead of linking the LLVM C++ libraries.

When `JOYEER_BUILD_UNITTESTS=ON`, CMake fetches GoogleTest 1.15.2. That is a
project test dependency, so the first unit-test configuration requires Git and
network access. Set `JOYEER_BUILD_UNITTESTS=OFF` when configuring without unit
tests or without network access.

## Source-build prerequisites

All platforms require:

- CMake 3.20 or newer;
- Ninja as the canonical generator;
- a host C and C++ compiler with C++20 support;
- a developer-installed Clang/LLVM 22.1.8 toolchain;
- the platform SDK and native linker inputs;
- Git and network access when unit tests need to fetch GoogleTest.

Use `clang` and the LLVM inspection tools from the same LLVM installation.
Mixing versions or package-manager prefixes is unsupported. The tools serve
these roles:

| Tool | Purpose |
|---|---|
| `clang` | Validate generated LLVM IR, generate machine code, and link `-o` output |
| `llvm-readobj` | Inspect object and executable debug sections in the full test suite |
| `lld-link` | Link Windows DWARF output |
| `llvm-pdbutil` | Inspect Windows PDB output in the full test suite |
| `dsymutil` | Produce and inspect macOS dSYM artifacts |

Only `clang` is required for ordinary native output. Missing inspection tools
cause their platform-specific tests to be omitted. Python, curl, LLVM headers,
and LLVM C++ libraries are not Joyeer source-build requirements.

For Windows x64, use the official
[`LLVM-22.1.8-win64.exe`](https://github.com/llvm/llvm-project/releases/download/llvmorg-22.1.8/LLVM-22.1.8-win64.exe)
installer. The `clang+llvm-22.1.8-x86_64-pc-windows-msvc.tar.xz` archive also
contains LLVM development libraries that Joyeer does not link and is therefore
unnecessary for normal development.

Platform prerequisites are:

| Platform | Required software |
|---|---|
| Windows | Visual Studio with Desktop development with C++, MSVC, a Windows SDK, CMake, and Ninja |
| Linux | GCC or Clang with C++20 support, libc development files, binutils, CMake, and Ninja |
| macOS | Xcode Command Line Tools and the active macOS SDK, CMake, and Ninja |

Windows uses the MSVC compiler toolset installed by Visual Studio for the
Joyeer implementation. CMake rejects MinGW, GCC, clang-cl, and other non-MSVC
compilers. Ninja is the canonical build generator on Windows, but it only
schedules compiler and linker commands; it does not replace `cl.exe` or the
Visual Studio toolchain. Platform SDKs, sysroots, startup objects, and system
libraries remain external prerequisites.

Before configuring, verify the tools supplied by your environment:

```text
cmake --version
ninja --version
clang --version
git --version
```

For the complete native debug test suite, also check `llvm-readobj`, plus
`lld-link` and `llvm-pdbutil` on Windows or `dsymutil` on macOS. On Windows,
run `cl` from a Visual Studio Developer shell as well. Ensure the reported
Clang and LLVM tool versions and installation paths match.

## Configure, build, and test

Run CMake from a shell in which the host compiler and platform SDK are active.
On Windows, use a Visual Studio Developer PowerShell or Developer Command
Prompt. The Windows presets explicitly select `cl.exe`; configuration fails
when the Visual Studio C++ workload or Windows SDK is missing.

```text
cmake -S . -B build -G Ninja \
  -DJOYEER_CLANG_EXECUTABLE=/absolute/path/to/clang
cmake --build build
ctest --test-dir build --output-on-failure
```

PowerShell uses a backtick instead of a backslash for line continuation, or the
configure command can be written on one line. Omit
`JOYEER_CLANG_EXECUTABLE` only when the intended Clang is discoverable through
`PATH` or one of CMake's platform hints.

Windows contributors may use the checked-in presets:

```text
cmake --preset x64-debug -DJOYEER_CLANG_EXECUTABLE=C:/absolute/path/to/clang.exe
cmake --build --preset x64-debug
ctest --preset x64-debug
```

The build is out-of-source only. The executable is under `build/bin/` for
single-config generators. Unfiltered CTest is the required final CMake gate;
labels are for focused iteration.

## Tool discovery

CMake searches `PATH` and common LLVM installation locations. Prefer an
explicit path when multiple LLVM installations exist:

```text
-DJOYEER_CLANG_EXECUTABLE=/absolute/path/to/llvm/bin/clang
```

The LLVM inspection tools are searched beside that executable and then on
`PATH`. On macOS, CMake obtains the active SDK from `xcrun`; use
`JOYEER_MACOS_SDK_PATH` only when an explicit SDK is necessary.

A configuration without Clang is allowed for frontend development and textual
LLVM emission. CMake reports that native integration tests are unavailable,
and `joyeer -o` reports a missing-tool diagnostic at runtime.

## Troubleshooting

- **CMake selects the wrong Clang:** clear the build directory or use
  `cmake --fresh`, then set `JOYEER_CLANG_EXECUTABLE` explicitly.
- **Windows rejects the compiler:** reopen a Visual Studio Developer shell and
  verify `cl` and the Windows SDK before running CMake.
- **Native debug tests are missing:** install the matching LLVM inspection
  tools and reconfigure so CMake can register those tests.
- **GoogleTest cannot be fetched:** restore Git/network access, provide it
  through the normal CMake dependency cache, or configure with
  `-DJOYEER_BUILD_UNITTESTS=OFF`.
- **macOS native linking cannot find system libraries:** run `xcode-select -p`
  and `xcrun --sdk macosx --show-sdk-path`, then repair or select Xcode Command
  Line Tools before reconfiguring.
