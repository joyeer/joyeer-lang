# Self-Contained Native Toolchain Distribution

> **Status:** Proposed. The current supported implementation still emits
> textual LLVM IR and invokes an externally configured Clang driver. This plan
> describes how to remove that user-facing LLVM installation dependency
> without creating a second language mode or backend pipeline.

## 1. Goals

The released Joyeer toolchain should:

- compile native programs without requiring the user to install LLVM or Clang;
- keep verified Joyeer IR and textual LLVM IR as the compiler/backend boundary;
- preserve the existing optimization, ownership, diagnostics, and debug
  behavior;
- isolate LLVM and linker crashes from the frontend process;
- ship relocatable, signed packages for each supported host and architecture;
- expose only Joyeer-owned interfaces whose compatibility policy Joyeer can
  control.

The first milestone targets macOS arm64. Windows and Linux follow the same
architecture with platform-specific linking and debug artifact handling.

This work does not make native compilation independent of the target operating
system. A final executable still needs a target SDK or sysroot, startup objects,
system libraries, and an ABI-compatible runtime. Removing the LLVM installation
dependency and providing a complete cross-compilation sysroot are separate
projects.

## 2. Decision

The default release architecture is two private executables:

```text
Joyeer source
  -> joyeer
       lexer / parser / semantic pipeline
       verified Joyeer IR
       textual LLVM IR
  -> joyeer-codegen
       LLVM IR parser and verifier
       optimization pipeline
       target object generation
       native runtime payload
       linker integration
  -> native executable + debug artifacts
```

`joyeer-codegen` statically links a pinned, reduced LLVM and LLD build. It is a
private implementation component, not a user-selected backend. The normal
`joyeer` command locates it relative to its own installation directory.

This is preferred over linking LLVM directly into `joyeer` because a malformed
module, LLVM assertion, fatal error, or linker failure then terminates only the
code-generation helper. The frontend can translate that failure into a stable
Joyeer diagnostic.

It is also preferred over distributing LLVM dynamic libraries. A release must
not inherit package-manager paths, LLVM C++ ABI compatibility, or a collection
of independently signed LLVM libraries.

Textual LLVM IR remains the boundary because it is already emitted, tested,
inspectable through `--emit-llvm`, and independent of LLVM's C++ object model.
The helper parses IR produced by the same pinned release with which it ships;
Joyeer does not promise that arbitrary LLVM IR from other releases is accepted.

## 3. Package Layout

### macOS

```text
Joyeer.pkg payload
  bin/
    joyeer
  libexec/joyeer/
    joyeer-codegen
  share/joyeer/licenses/
    LLVM-LICENSE.txt
    THIRD-PARTY-NOTICES.txt
```

Both programs are Mach-O executables. LLVM, the selected target backend, and
LLD are statically linked into `joyeer-codegen`. Target-specific Joyeer runtime
bitcode or objects should be embedded in the helper when practical; if they
remain separate, they live under `libexec/joyeer/runtime/<target-triple>/` and
are found relative to the helper rather than through an environment variable.

The normal package should not contain LLVM `.dylib` files. Expected dynamic
dependencies are Apple-provided system libraries such as `libSystem` and,
while the helper is implemented in C++, `libc++`.

Ship separate arm64 and x86_64 packages initially. A universal package nearly
doubles the LLVM payload and should be added only if distribution data shows
that its convenience justifies the size.

### Windows

```text
Joyeer installation
  bin/
    joyeer.exe
  libexec/joyeer/
    joyeer-codegen.exe
  share/joyeer/licenses/
    LLVM-LICENSE.txt
    THIRD-PARTY-NOTICES.txt
```

LLVM and LLD/COFF are statically linked into the helper. A static runtime
library choice should avoid requiring an additional Visual C++ redistributable
when licensing and platform policy allow it. PDB generation remains part of
the helper's contract.

### Linux

```text
Joyeer installation
  bin/
    joyeer
  libexec/joyeer/
    joyeer-codegen
  share/joyeer/licenses/
    LLVM-LICENSE.txt
    THIRD-PARTY-NOTICES.txt
```

LLVM and LLD/ELF are statically linked into the helper. The initial package may
target the host distribution's glibc ABI. A later portable Linux SDK can use a
versioned sysroot or a musl-based target; that is not required to eliminate the
LLVM dependency.

## 4. Component Responsibilities

### `joyeer`

The frontend owns:

- command-line parsing and source diagnostics;
- the syntax, semantic, type, control-flow, and ownership pipeline;
- Joyeer IR construction and verification;
- deterministic textual LLVM IR emission;
- output collision checks that can be performed before code generation;
- locating and launching the matching helper;
- converting helper responses, crashes, and version mismatches into stable
  diagnostics.

`joyeer` must not search the global `PATH` for an arbitrary compatible helper.
An explicit development-only override may exist in build/test infrastructure,
but it must not become a user-visible backend selection mode.

### `joyeer-codegen`

The helper owns:

- protocol and build-version validation;
- LLVM target initialization;
- parsing and verifying textual LLVM IR;
- target triple, data layout, CPU, and feature selection;
- the `-O0` through `-O3` optimization pipelines;
- target object generation;
- inclusion of the target-specific Joyeer native runtime;
- final native linking or invocation of the approved platform linker;
- PDB, DWARF, and dSYM artifact production;
- atomic output replacement and cleanup after failure;
- structured diagnostics without ad hoc terminal output.

The helper must not accept unrestricted linker arguments from source code or
environment variables. All options cross an allow-listed, typed protocol.

### Joyeer native runtime

The current C11 runtime is linked as a static archive by Clang. The packaged
backend should replace that file dependency with one of these representations:

1. target-specific LLVM bitcode embedded in `joyeer-codegen`, preferred when
   whole-program optimization is enabled;
2. target-specific object data embedded in the helper and materialized only for
   linking;
3. a private, versioned archive installed beside the helper as an intermediate
   migration step.

Each payload is keyed by an exact target triple and runtime ABI version. The
runtime continues to use the platform C ABI and system services; embedding it
does not make it freestanding.

## 5. Frontend/Helper Protocol

The process boundary is Joyeer-owned and explicitly versioned. At minimum, a
request carries:

```text
protocol version
frontend build ID
LLVM IR bytes
source path for debug metadata
output path
target triple
optimization level
debug level and format
runtime ABI version
```

The response carries:

```text
success or stable error category
structured diagnostic records
debug artifact path when one was produced
backend build ID and LLVM version
```

IR and diagnostics must use length-delimited transport or another structured,
binary-safe representation. Do not parse human-formatted LLVM stderr as the
protocol. All strings are UTF-8; the Windows helper performs UTF-16 conversion
at the operating-system boundary.

The initial implementation may use private temporary request/response files to
reduce migration risk. Files must be created with owner-only access, must not
follow attacker-controlled symlinks, and must be removed on success, failure,
or interruption. Standard input/output transport can replace them after the
protocol is covered by integration tests.

The helper rejects a request when its protocol version, frontend build ID, or
runtime ABI is incompatible. There is no attempt to load a different system
LLVM as a fallback.

## 6. Optional Dynamic Library API

A `joyeer-backend.dll`, `libjoyeer-backend.dylib`, or
`libjoyeer-backend.so` may be produced later for IDEs and third-party build
systems. It is not the default CLI deployment boundary.

That library must:

- expose a Joyeer-owned C ABI, never the LLVM C++ ABI;
- use fixed-width integers, byte spans, callbacks, and size-versioned structs;
- provide explicit allocation/disposal functions;
- keep C++ exceptions and LLVM objects inside the library;
- expose ABI and build-ID queries before compilation;
- statically contain its matching LLVM implementation.

Joyeer IR C++ classes, `std::string`, `std::filesystem::path`, and C++ standard
library ownership must not cross this ABI. The process helper should wrap the
same internal backend core so the two products cannot diverge semantically.

## 7. LLVM and LLD Build

Release CI builds LLVM from a pinned release tag or commit. System Homebrew,
APT, or Visual Studio LLVM layouts are development conveniences and are not
release inputs.

### Automation policy

All checked-in bootstrap, dependency, packaging, signing, and release workflow
logic is implemented once in cross-platform Python 3.9+ using the standard
library. Do not add paired `.sh` and `.ps1` implementations. Platform-specific
tool names and flags are data selected by Python, not separate workflow logic.

The implemented bootstrap foundation uses this source layout:

```text
bootstrap.py
pixi.toml
pixi.lock
scripts/
  toolchain.py
  toolchain_support/
    cli.py
    pixi.py
    process.py
    targets.py
.pixi/                         # ignored locked development environment
.deps/                         # ignored build state
  pixi/                        # optional bootstrapped Pixi executable
```

`bootstrap.py` is the discoverable first-run entry point. It downloads the
pinned Pixi executable when needed, enters the environment described by
`pixi.toml`/`pixi.lock`, and configures an out-of-source CMake + Ninja build.
Pixi owns dependency resolution, package verification, caching, and upgrades.
On Windows, the host compiler and linker are MSVC; Visual Studio's C++ workload
and Windows SDK remain system prerequisites.

`scripts/toolchain.py` is the ongoing human and CI entry point. The currently
implemented commands are:

```text
python3 bootstrap.py
python3 bootstrap.py --offline
python3 scripts/toolchain.py status
python3 scripts/toolchain.py build
python3 scripts/toolchain.py test
```

Each command automatically enters the locked Pixi environment. `test` runs the
Python automation tests, builds Joyeer, and runs unfiltered CTest. Source LLVM
fetching and reduced-SDK packaging remain design work; no placeholder commands
or unused implementation are checked in for them.

On Windows, the same file may be launched with `py -3` when `python3` is not an
installed command. This is an invocation spelling difference, not a second
implementation. CI calls the same subcommands as developers.

The Python implementation must:

- use `pathlib` for paths and never assume `/` or drive-letter spelling;
- launch tools with argument arrays through `subprocess.run(..., check=True)`;
- never use `shell=True`, shell pipelines, or generated command files;
- download only pinned HTTPS assets and verify SHA-256;
- use atomic temporary files and renames for downloads;
- emit the exact command, target triple, pinned revision, and build ID in
  verbose mode and machine-readable provenance;
- return nonzero on every failed or incomplete step.

`pixi.lock` records development tool binaries and transitive packages and never
implicitly selects the newest LLVM. Future release-tooling work must add its own
source/provenance lock when that source build is implemented; no unused source
lock is kept in the repository meanwhile.

Python is a developer, source-build, and release dependency. It is not included
in the Joyeer package and is not required to run `joyeer`, `joyeer-codegen`, or
programs compiled by Joyeer. Main CMake builds that consume a prepared LLVM SDK
must also remain usable without invoking Python automatically.

The reduced build should:

- enable `llvm` and `lld`, but not the Clang language frontend;
- build static position-independent component libraries;
- enable only the target backends contained in that package;
- disable tests, examples, benchmarks, documentation, and unused tools;
- disable optional compression, terminal, and XML dependencies unless the
  package deliberately includes them;
- strip release binaries after debug symbols have been archived;
- record the exact LLVM commit and CMake options in build provenance.

The required LLVM surface includes IR parsing and verification, the new pass
manager, target initialization, target-machine object emission, object-file
support, and the selected native target. LLD is built from the same source
revision. LLVM and LLD C++ interfaces may change between releases; those
changes are absorbed inside `joyeer-codegen` and never alter the frontend
protocol without an explicit Joyeer protocol revision.

Do not set a package-size promise until a reduced prototype has been measured.
The acceptance metric is the stripped and compressed Joyeer package, not the
size of an unmodified package-manager LLVM installation.

## 8. Linking and Platform SDK Boundary

Removing Clang has two distinct steps:

1. LLVM libraries replace Clang's IR parsing, optimization, and object-code
   generation.
2. LLD or an approved platform linker replaces Clang's linker-driver role.

The second step still needs platform inputs:

| Platform | Required target inputs |
|---|---|
| macOS | Apple SDK stubs/framework metadata, startup policy, system libraries |
| Windows | CRT startup objects, UCRT/system import libraries, subsystem policy |
| Linux | `crt*.o`, libc, dynamic loader, and a target-compatible sysroot |

For the first macOS arm64 milestone, Joyeer may require Xcode Command Line Tools
and use the selected Apple SDK while supplying its own LLVM code generator.
The implementation may initially invoke the Apple linker and `dsymutil`; these
are operating-system development tools, not an LLVM package dependency. A later
milestone can embed LLD/Mach-O and LLVM's DWARF linking support, but it still
must locate a legally and technically valid SDK.

Apple SDKs must not be copied into a Joyeer package without confirming Apple's
redistribution terms. A self-contained LLVM backend therefore does not imply a
redistributable Apple sysroot.

## 9. macOS Requirements

The macOS release must additionally enforce:

- no package-manager or build-machine absolute paths in load commands;
- no non-system `.dylib` dependency unless it is an intentional Joyeer-owned
  library using `@rpath` or `@loader_path`;
- arm64 and x86_64 artifact validation on their corresponding packages;
- helper signing before signing the containing package or application bundle;
- hardened-runtime and notarization validation for the final installer;
- dSYM creation, replacement, and stale-artifact cleanup equivalent to the
  current native integration tests.

`otool -L`, `otool -l`, `codesign --verify`, and Gatekeeper assessment belong
in release CI. A normal macOS package should contain no Linux `.so` files and
no LLVM `.dylib` files.

## 10. Failure and Security Model

- Launch the helper with an argument vector, never through a shell.
- Treat a signal, abnormal termination, malformed response, or timeout as a
  backend failure with a stable Joyeer diagnostic.
- Preserve the current source/output/runtime collision protections.
- Write objects and executables to a private temporary location, then rename
  them into place only after all validation succeeds.
- Refuse to overwrite directories, symlinks, compiler inputs, or unexpected
  debug-artifact paths.
- Bound request sizes and diagnostic sizes before allocation.
- Do not load plugins, passes, response files, or configuration from the
  working directory.
- Record backend and LLVM build IDs in verbose diagnostics and generated build
  metadata, without making output nondeterministic by default.

## 11. Migration Plan

Foundation completed on 2026-07-23:

- cross-platform Python 3.9 bootstrap and toolchain entry points;
- strict, versioned LLVM lock parsing and host target selection;
- proxy-aware, atomic, digest-verified LLVM source download;
- safe and serialized source extraction under `.deps/`;
- offline and dry-run modes plus unified Python/CMake/CTest validation;
- explicit macOS SDK discovery for the transitional external-Clang linker.

1. Introduce an internal code-generation request/result abstraction around the
   current linker call. Preserve all current diagnostics and artifact checks.
2. Define and test the versioned frontend/helper protocol independently of
   LLVM.
3. Build a pinned, reduced LLVM toolchain in CI and create
   `joyeer-codegen` with IR verification and object emission for macOS arm64.
4. package the Joyeer native runtime as target-specific bitcode or object data.
5. Add final linking and dSYM behavior, initially using the Apple SDK tools
   where required.
6. Run the existing unfiltered compiler/native suite through the helper and
   add clean-machine package tests.
7. Switch the single production path to the packaged helper and remove
   `JOYEER_CLANG_EXECUTABLE`, the configured runtime archive path, and the
   external-Clang linker implementation together.
8. Repeat the vertical slice for Windows x64, Linux x64, and then arm64 hosts.
9. Add the optional dynamic C API only after the process protocol and backend
   semantics are stable.

There must be no permanent user-visible switch between external Clang and the
packaged backend. During development, the unfinished helper remains an isolated
test target until it can replace the current path without weakening the normal
gate.

## 12. Acceptance Gates

The macOS arm64 cutover requires:

- the complete unfiltered CTest suite passes through `joyeer-codegen`;
- native JSON parsing, ownership cleanup, overflow, and bounds traps retain
  their current behavior;
- `-O0` through `-O3`, line tables, full debug data, and dSYM lifecycle tests
  pass;
- compilation succeeds when LLVM and Clang are absent from `PATH`;
- package binaries have no Homebrew, Cellar, build-tree, or source-tree load
  paths;
- dynamic dependencies are restricted to an explicit allow-list of macOS
  system libraries;
- helper absence, version mismatch, crash, and malformed output produce stable
  diagnostics and no partial artifacts;
- a clean macOS machine with the documented minimum system tools can install,
  compile, link, run, debug, and uninstall Joyeer;
- the release archive includes LLVM's Apache-2.0 WITH LLVM-exception license,
  applicable notices, and notices for bundled third-party components.

Later acceptance gates can remove the Xcode Command Line Tools requirement only
after the linker, dSYM implementation, and SDK/sysroot policy are all resolved.
