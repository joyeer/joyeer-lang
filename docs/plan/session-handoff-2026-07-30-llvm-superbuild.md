# Session Handoff: LLVM/LLD CMake Superbuild

**Date:** 2026-07-30
**Branch:** `feature/init_version`
**Remote:** `origin/feature/init_version`

## Objective

Make CMake own the pinned LLVM/LLD dependency lifecycle:

1. download and verify LLVM 22.1.8 source;
2. build and install a reduced static LLVM/LLD SDK;
3. consume `LLVMConfig.cmake` and `LLDConfig.cmake` from the main Joyeer build;
4. prove that Joyeer can compile and link against LLVM and the host LLD driver;
5. later migrate native code generation from textual IR plus external Clang to
   the linked APIs.

LLVM is not a Git submodule and is not added to the main build with
`add_subdirectory` or `FetchContent`.

## Current state

Implemented:

- `cmake/superbuild/CMakeLists.txt` defines separate `JoyeerLLVM` and `Joyeer`
  `ExternalProject` builds.
- LLVM source is pinned to `llvm-project-22.1.8.src.tar.xz` with SHA-256
  `922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888`.
- `cmake/superbuild/DownloadLLVM.cmake` uses curl HTTP/1.1 with retries and
  resume support, then CMake verifies SHA-256 and extracts the archive.
- The reduced SDK enables LLD, builds only the native target, uses static
  LLVM/LLD libraries, and disables tests, examples, documentation, unused
  optional dependencies, and default tool builds.
- The main project supports `JOYEER_USE_LLVM_SDK=ON` and requires exact LLVM
  and LLD version 22.1.8.
- `JoyeerLLVMSDK` carries LLVM headers, definitions, selected LLVM component
  libraries, and the host LLD driver (`lldCOFF`, `lldELF`, or `lldMachO`).
- `JoyeerLLVMBackend` privately links `JoyeerLLVMSDK` in SDK mode.
- `JOYEER_VERIFY_LLVM_SDK=ON` builds `JoyeerLLVMSDKProbe`, which parses LLVM IR
  through LLVM APIs and references the host LLD driver API.
- Root `out/` is ignored so source archives and multi-gigabyte build products
  cannot be committed accidentally.
- Build documentation now covers prerequisites, full Superbuild use, staged
  targets, and prepared SDK use.

Still transitional:

- `JoyeerLLVMBackend` still emits textual LLVM IR with the existing custom
  emitter.
- Native executable output still invokes the configured external Clang driver.
- The planned Joyeer-owned dynamic backend C ABI is not implemented.
- Pixi remains available as a convenience source for current development tools.

## Validation completed

The following checks passed on Windows x64 with Visual Studio MSVC 19.51,
CMake 4.4.0, Ninja 1.13.2, and curl 8.21.0:

- Superbuild configure succeeds.
- The default main build configures with `JOYEER_USE_LLVM_SDK=OFF`.
- SDK mode fails clearly when `LLVM_DIR`/`LLD_DIR` are missing.
- The official 159.3 MB LLVM archive downloaded successfully through the
  resumable download script.
- CMake verified the exact pinned SHA-256 and extracted one source root.
- All edited CMake/C++/Markdown files have no editor diagnostics.
- `git diff --check` passes.

Before this Superbuild change, commit `448e9f1` passed Python tests 7/7 and the
full MSVC CTest suite 337/337. The full suite has not been rerun with the SDK
because LLVM has not yet been configured or compiled.

## Exact stopping point

LLVM source download, verification, and extraction completed. LLVM's own CMake
configure step has **not** completed, and no LLVM library has been compiled or
installed yet. `JoyeerLLVMSDKProbe` has therefore not been compiled or run.

Local `out/` contents are ignored and are not part of the handoff commit. A new
device starts with a fresh download unless its build cache is transferred
separately.

## Continue on another Windows device

Install and place on `PATH`:

- Visual Studio with Desktop development with C++ and a Windows SDK;
- CMake 3.20 or newer;
- Ninja;
- curl 7.71 or newer;
- Python 3.8 or newer for the LLVM source build.

Use a Visual Studio Developer PowerShell or Developer Command Prompt:

```pwsh
git fetch origin
git switch feature/init_version
git pull --ff-only origin feature/init_version

cmake -S cmake/superbuild -B out/superbuild -G Ninja
cmake --build out/superbuild --target JoyeerLLVM-download
cmake --build out/superbuild --target JoyeerLLVM-configure
```

The next discriminating checkpoint is `JoyeerLLVM-configure`. If it succeeds,
inspect `out/superbuild/llvm-build/CMakeCache.txt`, then continue:

```pwsh
cmake --build out/superbuild --target JoyeerLLVM
cmake --build out/superbuild --target Joyeer
ctest --test-dir out/superbuild/joyeer -L llvm-sdk --output-on-failure
```

A full build is equivalent to:

```pwsh
cmake --build out/superbuild
```

LLVM is large. Allow substantial build time, several gigabytes of disk space,
and enough memory for parallel C++ compilation. Link parallelism is limited to
one by default through `JOYEER_LLVM_PARALLEL_LINK_JOBS`.

## Prepared SDK path

After `JoyeerLLVM` installs the SDK, the main project can be tested directly:

```pwsh
cmake -S . -B out/joyeer-sdk -G Ninja `
  -DJOYEER_BUILD_UNITTESTS=ON `
  -DJOYEER_USE_LLVM_SDK=ON `
  -DJOYEER_VERIFY_LLVM_SDK=ON `
  -DLLVM_DIR="$PWD/out/superbuild/llvm-sdk/lib/cmake/llvm" `
  -DLLD_DIR="$PWD/out/superbuild/llvm-sdk/lib/cmake/lld"
cmake --build out/joyeer-sdk
ctest --test-dir out/joyeer-sdk -L llvm-sdk --output-on-failure
ctest --test-dir out/joyeer-sdk --output-on-failure
```

Keep the LLVM SDK and Joyeer build on compatible MSVC runtime settings. The
Superbuild defaults both Release builds to the MSVC DLL runtime.

## Known issues and first checks

1. The LLVM configure step is the first unvalidated stage. If it rejects a
   CMake option, inspect the command in
   `out/superbuild/JoyeerLLVM-prefix/src/JoyeerLLVM-stamp/` and update only the
   owning Superbuild option.
2. The initial CMake/libcurl GitHub download stalled twice with Windows
   Schannel HTTP/2 error 56. Do not restore `ExternalProject URL`; the custom
   downloader fixed this by using curl HTTP/1.1, retries, and resume.
3. Validate that `LLVM_BUILD_TOOLS=OFF`, `LLD_BUILD_TOOLS=OFF`, and
   `LLVM_INCLUDE_UTILS=OFF` still install every CMake export and static library
   required by `JoyeerLLVMSDK`. Relax only the option proven to block install.
4. If `JoyeerLLVMSDKProbe` fails to link, inspect the reduced SDK's exported
   LLD target dependencies before adding libraries manually.
5. Do not start migrating emitter behavior until the probe and unfiltered CTest
   both pass against the installed SDK.
6. Do not remove Pixi until the Superbuild passes on Windows, Linux, and macOS
   and the production native path no longer invokes external Clang.

## Files in this handoff

- `.gitignore`
- `CMakeLists.txt`
- `cmake/superbuild/CMakeLists.txt`
- `cmake/superbuild/DownloadLLVM.cmake`
- `lib/backend/CMakeLists.txt`
- `lib/backend/llvm_sdk_probe.cpp`
- `README.md`
- `AGENTS.md`
- `docs/building.md`
- `docs/plan/session-handoff-2026-07-30-llvm-superbuild.md`
