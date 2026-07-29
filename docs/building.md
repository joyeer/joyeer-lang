# Building Joyeer

This document defines the source-build prerequisites and dependency ownership
policy. It distinguishes tools required before CMake can run from project
dependencies that CMake owns.

## Dependency policy

Source builders install the build driver, generator, host compiler, and platform
SDK. CMake validates and uses those prerequisites; it does not install them.

CMake owns project dependencies:

- GoogleTest for C++ unit tests;
- the pinned LLVM and LLD SDK used by the packaged native backend;
- installation and packaging of Joyeer-owned binaries and libraries.

LLVM is not a Git submodule and must not be added to the main build with
`add_subdirectory` or `FetchContent_MakeAvailable`. The planned dependency path
is a CMake superbuild: `ExternalProject_Add` obtains a release archive pinned by
version and SHA-256, builds a reduced LLVM/LLD SDK in a separate build tree, and
then configures the main Joyeer project against that SDK with `find_package`.
A prepared SDK may be supplied instead for offline and iterative builds.

The superbuild is not implemented yet. During the transition, native output and
its integration tests use an externally installed Clang/LLVM 22 toolchain.
The existing Pixi bootstrap remains a convenience path until the CMake-managed
LLVM SDK replaces it; it is not the target dependency boundary.

## Source-build prerequisites

All platforms require:

- CMake 3.16 or newer;
- Ninja as the canonical generator;
- a host C and C++ compiler with C++20 support;
- network access for the first dependency population, or a populated CMake
  download cache and prepared LLVM SDK for an offline build;
- enough local resources to build LLVM when a prepared SDK is unavailable.

Python 3.9 or newer is required only for the current `bootstrap.py`,
`scripts/toolchain.py`, and their automation tests. A direct CMake build does
not require Python, and released Joyeer tools must not require it.

Platform prerequisites are:

| Platform | Required software |
|---|---|
| Windows | Visual Studio with Desktop development with C++, MSVC, a Windows SDK, CMake, and Ninja |
| Linux | GCC or Clang with C++20 support, libc development files, binutils, CMake, and Ninja |
| macOS | Xcode Command Line Tools and the active macOS SDK, CMake, and Ninja |

Windows uses MSVC for the Joyeer implementation. MinGW is not a supported
fallback. Platform SDKs, sysroots, startup objects, and system libraries remain
system prerequisites even after LLVM and LLD are built by CMake.

During the external-Clang transition, install Clang/LLVM 22.1.8 as well.
`clang` is required for native output, `lld-link` is required for Windows DWARF
output, and LLVM inspection tools are required for complete backend/debug test
coverage. Pass a non-default Clang location through
`JOYEER_CLANG_EXECUTABLE`.

## Configure, build, and test

Run CMake from a shell in which the host compiler and SDK are active. On
Windows, use a Visual Studio Developer PowerShell or Developer Command Prompt.

```text
cmake -S . -B build -G Ninja \
  -DJOYEER_CLANG_EXECUTABLE=/absolute/path/to/clang
cmake --build build
ctest --test-dir build --output-on-failure
```

PowerShell uses a backtick instead of a backslash for line continuation, or the
configure command can be written on one line. Omit
`JOYEER_CLANG_EXECUTABLE` only when the intended Clang is discoverable through
`PATH` or the platform locations encoded by the current CMake build.

The build is out-of-source only. The executable is under `build/bin/` for
single-config generators. Unfiltered CTest is the required final CMake gate;
labels are for focused iteration.

The transitional convenience path remains:

```text
python3 bootstrap.py
python3 scripts/toolchain.py test
```

On Windows, use `py -3` in place of `python3` when necessary.

## Planned LLVM superbuild

The dependency build and the main project must remain separate CMake configure
steps. The superbuild will:

1. download an exact LLVM release archive and verify its SHA-256;
2. build `llvm` and `lld` without Clang, tests, examples, or documentation;
3. build static position-independent LLVM/LLD component libraries for the
   selected package targets;
4. install `LLVMConfig.cmake`, `LLDConfig.cmake`, headers, libraries, licenses,
   and provenance into a private SDK prefix;
5. configure Joyeer with that prefix in `CMAKE_PREFIX_PATH`.

The main project will consume the prepared SDK rather than download sources:

```cmake
find_package(LLVM 22.1.8 EXACT CONFIG REQUIRED)
find_package(LLD 22.1.8 EXACT CONFIG REQUIRED)
```

The Joyeer LLVM backend is a Joyeer-owned dynamic library with a versioned C
ABI. LLVM and LLD are private implementation details statically linked into
that library; LLVM C++ types, exceptions, allocators, and standard-library
objects never cross the ABI. The CLI may invoke a private helper that loads the
same backend library to preserve process-level crash isolation.

## Release users

A released Joyeer package contains the compiler, the matching Joyeer LLVM
backend, the native runtime payload, licenses, and any private helper. Release
users do not install CMake, Ninja, Python, Clang, LLVM, or LLD. They may still
need operating-system SDK components when native linking requires inputs that
cannot legally or technically be redistributed.
