# Restricted C Interoperation

> **Status:** the initial runtime-only `Int`/`Void` import/export slice is
> implemented. The broader scalar design in [§19](../spec/19-c-interop.md)
> is not fully implemented. Native addresses, output pointers, C aggregates,
> callbacks, variadic calls, and public user FFI remain unsupported.

## 1. Current subset

| Surface | Current support |
|---|---|
| Import | `@c_extern(symbol: "...")` on a bodyless top-level function |
| Export | `@c_export(symbol: "...")` on a top-level Joyeer definition |
| Native parameters | Default or explicit `borrowing Int`, passed as C `int64_t` |
| Native result | Explicit `Int`, `Void`, or `()`; the latter two map to C `void` |
| Empty parameter list | A fixed C11 `(void)` prototype |
| Source names | Ordinary labels, optional separate internal parameter names, and visibility |
| Authorization | Explicit named-unit selection with `--runtime-module` |
| Object output | `--emit-runtime-object`, without executable startup or a runtime archive |

The implementation enforces the declaration, annotation, and symbol rules
in §19 for this subset. It rejects all other native parameter/result types
instead of approximating their representations. In particular, ordinary
Joyeer `Bool` and `UInt8` support does not make them usable in C signatures
yet, and the broader integer/floating types are not added to the prelude.

This does not migrate any production runtime algorithm from C to Joyeer.
Existing host builtins and normal executable generation remain supported.

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

This is ordinary link-time selection of a dynamic dependency. It does not
implement `dlopen`, `LoadLibrary`, function-address lookup, header import,
or native dependency fields in joypm manifests. Missing required native
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

Object emission uses the existing versioned backend C ABI and opaque module
handle. The private IR wire format is version 2; the backend C ABI remains
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
boundaries, zero and twelve arguments, `void` results, two independently
compiled Joyeer objects, archive extraction, missing native definitions,
ordinary-source rejection, and output-path failures.
Where `llvm-nm` is available, object checks require exactly the approved C
exports/imports and reject incidental startup or runtime dependencies.

Full acceptance uses unit tests and joypm enabled and unfiltered CTest;
focused labels do not replace that gate. Native results for one host do not
establish acceptance on other platforms or a complete implementation of §19.

### Native validation evidence

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

These results validate the existing `Int`/`Void` slice, not the broader scalar
design or runtime migration. Linux and Windows ARM64 C interop acceptance
remain separate native gates.
