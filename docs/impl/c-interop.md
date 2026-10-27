# Restricted C Interoperation

> **Status:** authorized scalar imports/exports implement every representation
> in [§19.4](../spec/19-c-interop.md#194-c-scalar-representations).
> Imports also support the restricted buffer ABI below. Platform acceptance
> is recorded separately below. General native addresses/output pointers, C aggregates,
> callbacks, variadic calls, and public user FFI remain unsupported.

## 1. Current scalar boundary

| Surface | Current support |
|---|---|
| Import | `@c_extern(symbol: "...")` on a bodyless top-level function |
| Export | `@c_export(symbol: "...")` on a top-level Joyeer definition |
| Native parameters | Borrowing scalars; imports additionally accept the buffer forms below |
| Native result | Any supported scalar, or explicit `Void` / `()` mapping to C `void` |
| Empty parameter list | A fixed C11 `(void)` prototype |
| Source names | Ordinary labels, optional separate internal parameter names, and visibility |
| Authorization | Explicit named-unit selection with `--runtime-module` |
| Object output | `--emit-runtime-object`, without executable startup or a runtime archive |

| Joyeer scalar | C representation |
|---|---|
| `Bool` | `_Bool` |
| `Int8`, `Int16`, `Int32`, `Int64` | Corresponding `intN_t` |
| `UInt8`, `UInt16`, `UInt32`, `UInt64` | Corresponding `uintN_t` |
| `Int`, `UInt` | `int64_t`, `uint64_t` |
| `Float`, `Double` | IEEE 754 C `float`, `double` |

The implementation enforces the declaration, annotation, and symbol rules
in §19. All these types are available in the prelude. Source types remain
distinct: for example, an `Int64` value is not implicitly converted to `Int`.
Repeated declarations of one C symbol compare native representations, so
`Int` and `Int64` signatures agree, as do `UInt` and `UInt64`. Signedness,
width, Boolean, and floating representations cannot be interchanged.
All other native parameter/result types are rejected, except for the
explicit buffer-import extension below.

Native declarations, export adapters, and call sites carry the target's
required narrow-integer attributes. On x86-64 Unix and macOS ARM64, narrow
signed/unsigned values use `signext`/`zeroext`; Windows x64 requires
`zeroext` for `_Bool` but not 8/16-bit integers. Windows and Linux ARM64
use neither extension attribute. Fixed-prototype `Float` stays `float`,
not a variadically promoted `double`. These rules also apply to
compiler-generated C runtime calls. Native targets outside the admitted
x86-64/ARM64 Windows MSVC, Linux, and macOS families are diagnosed.

This does not migrate any production runtime algorithm from C to Joyeer.
Existing host builtins and normal executable generation remain supported.

### Buffer imports

Only `@c_extern` admits these additional parameter forms:

| Joyeer parameter | C ABI parameters, in source-parameter order |
|---|---|
| borrowing `String` or `[UInt8]` | `const uint8_t* data, int64_t count` |
| borrowing `[String]` | `const JoyeerString* data, int64_t count` |
| borrowing `[[UInt8]]` | `const JoyeerArray* data, int64_t count` |
| initializing `String` | `JoyeerString* output` |
| initializing `[UInt8]` or `[String]` | `JoyeerArray* output` |

Counts are byte counts for strings/byte buffers and element counts for
lists. There is no implicit C-string conversion or NUL terminator.
The native declarations come from `src/runtime/native.h` and `runtime.h`.
List descriptors expose only borrowed runtime handles: the native function
must not mutate, retain, or free descriptors or backing storage.

Output arguments require the ordinary `&` marker and uninitialized owning
storage. Before the native call, generated code constructs valid empty output
handles. Native implementations replace them using
`joyeer_native_bytes_assign`, `joyeer_native_string_assign`, or
`joyeer_native_strings_assign`. Those helpers copy through the runtime
allocator and preserve clone/destruction metadata. Do not assign foreign
allocations into an output handle. Every return, including failure, leaves
valid destroyable outputs; a status-returning Joyeer wrapper decides whether
to expose them. Allocation failures follow the existing runtime panic policy.

Existing exclusivity, definite-initialization and lifetime rules still apply.
`inout`/`consuming` buffers, nested output byte arrays, buffer return values,
buffer exports, arbitrary aggregate ABI, escaping pointers and callbacks are
not supported. Repeated native declarations must agree on buffer shape and
access effect, even when two flattened pointer signatures happen to match.
The interface is revision-matched to the compiler/runtime; version 1 is
identified by `JOYEER_NATIVE_BUFFER_ABI_VERSION`, not a stable ABI for every
internal runtime operation.

## 2. Explicit runtime compilation

The following is accepted in an explicitly authorized named unit:

```joyeer
@c_extern(symbol: "native_identity")
func nativeIdentity(value: Int): Int

@c_export(symbol: "runtime_identity")
private func runtimeIdentity(input value: Int): Int {
    return nativeIdentity(value: value)
}
```

For a file named `bindings.joyeer`, choose a new output path whose parent
directory already exists:

```text
joyeer --module-name runtime.core --runtime-module runtime.core --emit-runtime-object runtime.o bindings.joyeer
```

Use an appropriate `.obj` name on Windows. `--runtime-module <name>` is
repeatable; every name must identify a distinct explicitly supplied unit.
The named root must be authorized for runtime-object output. Legacy unnamed
single-file mode cannot grant runtime context.

In the service API, `CompileOptions.runtimeModules` selects those names and
`OutputMode::runtimeObject` selects object emission. Each reachable
`semantic::ModuleInput` receives its own context flag, which the semantic
model records by source identity. Imports, dependencies, directory names,
`Foundation`, and `public` do not propagate or infer authorization.
The flag is a deliberate toolchain input, not a security sandbox.

Ordinary units may call unannotated checked Joyeer wrappers, but may not
declare or directly call annotated bindings. Selecting a unit for runtime
compilation does not grant permission to its dependencies automatically.
`-o` still requires a valid application `main`; C exports are not executable
entry points. Runtime-object emission deliberately omits the generated
`joyeer_main` adapters even when an ordinary function is named `main`.

## 3. Static and dynamic providers

The annotation specifies the C symbol and derives its signature from the
source declaration. It does not name an archive, shared library, import
library, search path, or loader policy. The build target supplies those.

The durable [CMake fixture](../../tests/c-interop/CMakeLists.txt) illustrates
the ownership of each step:

1. Invoke the build-tree compiler with an explicit source list and runtime
   context to produce Joyeer objects.
2. Archive the objects and link a C caller against them.
3. Select either `CInteropNativeStatic` or `CInteropNativeShared` with
   `target_link_libraries`, without changing any Joyeer import declaration.

On Windows, the shared provider exports its C functions and CMake supplies
the DLL's import library; the test executable and DLL are deployed together
in the build's executable directory. On Unix-like targets, CMake supplies
the shared-library dependency and build-tree runtime search path. Installed
library locations, RPATH/install names, and deployment remain build/package
responsibilities, not per-function annotation arguments.

The compiler also accepts repeatable `--link-library <file>` arguments.
Each path resolves against the compiler working directory, must be a regular
file, and is protected against output/PDB cleanup collisions. No library-name
search or shell flags are inferred. COFF/Mach-O receive explicit inputs;
ELF groups the native archives and runtime to resolve inter-archive references.
Files are checked in validation/text/object modes too, but linking occurs only
for executable output. Native provider deployment remains the caller's job.
joypm's optional module fields are described in [its manifest contract](joypm.md).

This is ordinary link-time selection of a dynamic dependency. It does not
implement `dlopen`, `LoadLibrary`, function-address lookup, header import,
or native dependency fetching/building in joypm. Missing required native
definitions fail at the native link; source checking cannot prove a native
library's actual prototype agrees.

## 4. Pipeline and isolation

The parser stores spanned annotation names and arguments on function syntax.
It permits an omitted body on an annotated declaration so semantic checking
can diagnose the actual import/export rule. Ordinary declarations still
require bodies. This parser support does not implement the proposed
`@spec`/`@property` shorthand or DSL.

Type checking validates annotation arguments, explicit results, authorized
declarations/calls, admitted signatures, compatible repeated imports,
import/export signature agreement, and unique exports. It records a
`NativeBinding` alongside the source callable signature; no second signature
configuration or per-C-function compiler case is required.
The added stable diagnostic IDs are:

- `type-checking.invalid-annotation`
- `type-checking.invalid-native-declaration`
- `type-checking.unsupported-native-signature`
- `type-checking.unauthorized-native-binding`
- `type-checking.conflicting-native-symbol`

Joyeer IR carries that binding and each function's runtime context. The
shared verifier checks native signatures, body/external consistency,
authorization, calls, and symbol conflicts again before either backend
accepts the module. Native imports have no invented body or source-local
debug scopes; their annotation span is retained in the binding. Definitions
keep their ordinary source/debug locations.

LLVM native imports use their C linkage names directly. Exports have a
separate C-callable adapter that invokes the Joyeer implementation. C names
are reserved before generated private names, avoiding accidental linker-name
suffixes. Runtime objects keep source implementations and ownership helpers
internal, and emit managed helpers only for types actually used by their
definitions and stored components. A leaf scalar object therefore does not
acquire runtime dependencies merely from prelude signatures.

Explicit C bindings and compiler-generated runtime calls share the backend's
C declaration check. Reusing a symbol with a different LLVM function type or
calling convention fails with `llvm.conflicting-native-symbol`, anchored to
the explicit binding when present. This includes runtime calls introduced by
printing, checked arithmetic, and ownership helpers. Compatible runtime
symbols remain usable for imports and exports; the `joyeer_` prefix is not
blanket-reserved. This check covers declarations known to the compiler, not
the prototypes in an external native library.

Object emission uses the existing versioned backend C ABI and opaque module
handle. The private IR wire format is version 4; the backend C ABI remains
version 1. The compiler and backend must be built/deployed from the same
revision. macOS objects use the configured deployment target rather than a
newer default OS version inferred from the host running LLVM.

## 5. Build and acceptance

The `joyeer` target builds the C++ compiler/backend without waiting for
`JoyeerRuntime`. The `JoyeerToolchain` `ALL` target separately stages
the archive beside the compiler. joypm bootstrap depends on that staged
product. This supplies an acyclic foundation for a future mixed runtime;
the production archive still contains only its existing C implementation.

After the normal configure with testing enabled:

```text
cmake --build build
ctest --test-dir build --output-on-failure -R "CInterop|JoyeerCInterop"
ctest --test-dir build --output-on-failure
```

The [unit tests](../../tests/unittests/compiler/cinterop_test.cpp) cover
syntax, semantic validation, module authorization, lowering, wire transport,
malformed native IR, symbol conflicts, and debug-preserving emission.
The [native fixtures](../../tests/c-interop/) cover C/Joyeer/C calls with
static and shared providers at `-O0 -gfull` and `-O2 -g0`, signed-64-bit
boundaries, zero and twelve arguments, `void` results, multiple independently
compiled Joyeer objects, archive extraction, missing native definitions,
ordinary-source rejection, and output-path failures.
Conflicts with generated runtime signatures are rejected before object
emission at both `-O0` and `-O2`.
Where `llvm-nm` is available, object checks require exactly the approved C
exports/imports and reject incidental startup or runtime dependencies.

The added scalar fixtures cover every signed/unsigned width, both default
64-bit types, `_Bool`, `float`, and `double`. They exercise unsigned high bits,
floating subnormals/extrema, signed zero, infinities, NaNs without payload
assumptions, exact-precision literals, and 21 mixed integer/floating arguments
that exhaust both register classes. Target-derived bindings use real C
`long`, `unsigned long`, plain `char`, `size_t`, `ptrdiff_t`, and Windows
`BOOL` prototypes. Configure determines plain-char signedness with the target
C compiler, and C assertions reject stale or unsupported data-model assumptions.

Separate native scalar programs check field/array/optional storage, printing,
contextual literals in conditional/match blocks, unsigned wrapping, and signed
overflow/division traps in both Joyeer optimization profiles. Unit tests also
reject out-of-range literals, implicit numeric conversions, malformed floating
IR, and conflicting native representations.

Full acceptance uses unit tests and joypm enabled and unfiltered CTest;
focused labels do not replace that gate. Native results for one host do not
establish acceptance on other platforms or a complete implementation of §19.

### Native validation evidence

The macOS ARM64 and Windows x64 records below describe the historical
`Int`/`Void` slice. Complete scalar validation is recorded separately for
Windows ARM64.

#### macOS ARM64

On native macOS ARM64 with AppleClang 21.0.0.21000334, LLVM/LLD 22.1.8,
and the already installed macOS 26.5 SDK/deployment target:

- The compiler-only/native fixture targets built and ran before a Joyeer
  runtime archive had been constructed.
- The complete Debug default build passed with unit tests and joypm enabled;
  an incremental build required only product staging.
- All 30 `c-interop`-labeled tests passed, including static/shared providers
  and both Joyeer optimization profiles.
- Final unfiltered serial CTest completed 818 registered tests: 817 passed
  and one existing non-UTF-8 directory-entry test was skipped by its platform
  guard.

The active macOS 27.0 SDK's `arm64e.x1` TAPI entries are not readable by this
LLVM/LLD version; that configuration failed native linking before selecting
the installed 26.5 SDK. A parallel unfiltered run also exposed an existing
filesystem-test temporary-directory collision: those tests do not include
process identity in their directory names. No unrelated runtime/test code
was changed; the final unfiltered acceptance above used `-j 1`.
A full native macOS Release compiler build remains a separate gate.

#### Windows x64

On native Windows x64 on 2026-10-19, with Windows 11 Enterprise build 26300,
MSVC 19.51.36260 (toolset 14.51.36231), Windows SDK 10.0.28000.0,
LLVM/LLD 22.1.8, CMake 4.4.4, and Ninja
1.13.0.git.kitware.jobserver-pipe-1:

- Fresh build trees at `out\build\c-interop-x64-debug` and
  `out\build\c-interop-x64-release` used the x64 presets with `BUILD_TESTING`,
  `JOYEER_BUILD_UNITTESTS`, and `JOYEER_BUILD_JOYPM` enabled, and
  `INSTALL_GTEST=OFF`.
- Before building a runtime archive in the Debug tree, the compiler,
  C interop unit tests, and static/shared round-trip targets built, and all
  30 `c-interop` tests passed, including both Joyeer optimization profiles.
- Both complete default `ALL` builds passed, including joypm bootstrap.
  Subsequent incremental builds required only product staging.
- Unfiltered serial CTest passed **846/846 in Debug** and
  **846/846 in Release**, with no skipped tests. Each run included all
  30 C interop tests and all 15 joypm gates, including self-build and
  relocated component installation.
- The in-memory module regression covers no debug information, full DWARF,
  and full CodeView, each generating objects at `-O0` through `-O3` while
  preserving the original printed module. Before the fix, both full-debug
  cases reproduced the `0xc0000005` object-generation failure with managed
  locals. Normalizing debug records only in the code-generation clone fixes
  those cases and the real Debug joypm bootstrap without serializing LLVM IR.
- PE headers independently identify the compiler, backend, and joypm as
  `IMAGE_FILE_MACHINE_AMD64` in both configurations. Debug bootstrap also
  produced the joypm PDB.

These results validate the initial `Int`/`Void` slice, not the subsequent
complete scalar implementation or runtime migration.

#### Windows ARM64: complete scalars

On native Windows ARM64 on 2026-10-20, using MSVC 19.51.36260,
Windows SDK 10.0.28000.0, and LLVM/LLD 22.1.8:

- Both `arm64-debug` and `arm64-release` complete default builds passed,
  including runtime staging and direct joypm bootstrap, with unit tests and
  joypm enabled. The Release tree used the installed SDK and existing pinned
  dependency sources, not a downloaded host toolchain.
- Final unfiltered serial CTest passed **871/871 in Debug** and
  **871/871 in Release**, with no skipped tests. Each run included all
  **50 C interop gates** and all **15 joypm gates**, including self-build
  and relocated installation.
- Both static and shared providers passed the complete scalar round trips
  at `-O0 -gfull` and `-O2 -g0`, in each host build configuration.
- PE headers independently confirmed ARM64 compiler products and a
  representative optimized shared-provider caller.
- SDK Clang IR inspection agreed with the narrow-extension rules for
  x86-64 and ARM64 on Windows, Linux, and macOS. This is ABI inspection,
  not native execution on those other hosts.

Complete scalar native acceptance on Windows x64, Linux, and macOS remains
open until measured on those targets. These gates do not establish native
memory safety, general user FFI, or production runtime migration.
