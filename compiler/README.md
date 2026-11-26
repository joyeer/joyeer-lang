# Reading the Joyeer compiler

Start at [`CompilerService::compile`](driver/compiler+service.cpp), specifically the
overload taking `ModuleInput`s. It shows which model each stage consumes, where its
diagnostics go, and which failures stop the next stage. [`Driver::run`](driver/driver.cpp)
handles the output mode and, for native output, hands the result to the linker.

| Question | First place to read | What crosses the boundary |
|---|---|---|
| Which files are compiled? | [`CompilerService::compileModules` and `parse`](driver/compiler+service.cpp), [toolchain](driver/toolchain.cpp) | Reachable modules, their parsed files, and a source table indexed by `SourceSpan::sourceId`. Runtime authorization belongs to the source unit, not its importer. |
| What did the user write? | [Lexer](frontend/lexparser.cpp), [`Parser::parse`](frontend/parser.cpp), [syntax nodes](frontend/syntax.h) | Tokens with spans become a syntax tree. Platform selection [filters declarations](frontend/platform.cpp) before names are bound. |
| Which declaration does a name denote? | [`NameResolver::resolveWithPrelude`](frontend/nameresolution.cpp), [`SemanticModel`](frontend/semantic.h) | Scopes, symbols, references, and a list of references that need a receiver or contextual type. The resolver brings the bundled prelude sources into this compilation. |
| What type and call target does an expression have? | [`TypeCheckingBuilder::build`](frontend/typechecking.cpp), [type checker source map](../docs/impl/type-checking.md#1-boundary) | `TypeCheckedModel` overlays the syntax and name bindings with canonical types, completed references, signatures, and generic instances. |
| Is an otherwise well-typed use valid on this path? | [`Analyzer::analyze`](frontend/semanticanalysis.cpp) | Control-flow, initialization/consumption, and usage diagnostics. Errors stop lowering; warnings do not. |
| Which operations and cleanups must run? | [`Lowerer::lower`](ir/irlowering.cpp), [lowering walkthrough](ir/lowering/internal.h), [IR verifier](ir/verifier.cpp) | A backend-neutral `ir::Module`: typed functions, basic blocks, ownership operations, and source/debug information. |
| Where does LLVM start? | [Compiler-side `Emitter::emit`](codegen/llvm.cpp), [C ABI entry](backend/native_module.cpp), [native builder](backend/native_types.cpp) | Private Joyeer IR bytes go in; an opaque LLVM module handle comes back. `--emit-llvm` prints it. Native output uses the handle directly, not the printed text. |
| What becomes the executable? | [Link inputs](codegen/linker.cpp), [object generation and LLD](backend/backend.cpp), [runtime build](../lib/Runtime/CMakeLists.txt) | A native object linked with `JoyeerRuntime` and the selected native libraries. LLVM and LLD run in process. |

## Three distinctions that matter when changing code

- **A role is not a type name.** For example, `[T]` resolves to the `array_type`
  declaration in the [collections prelude](../lib/Runtime/collections.joyeer), not
  to whichever declaration is spelled `Array`. Its ordinary generic fields and
  methods use the same type machinery as user declarations. Start with
  [role validation](frontend/typechecking/prelude.cpp) for compiler/library contracts.
- **Checking a move is not emitting a destructor.** The [type checker](frontend/typechecking/storage.cpp)
  checks whether an expression can be consumed; [semantic analysis](frontend/semanticanalysis.cpp)
  checks whether that storage is still live on a given path. [IR lowering](ir/lowering/ownership.cpp)
  emits the cleanups, and the [backend](backend/native_ownership.cpp) implements them for the
  concrete layout. A change at one layer does not replace the others.
- **The compiler core and backend are separate builds.** [`JoyeerCompilerCore`](CMakeLists.txt)
  is a private object target shared by the CLI and unit tests. [`JoyeerBackend`](backend/CMakeLists.txt)
  builds the verifier again with its own runtime and checks the decoded IR before LLVM emission.
  This is deliberate: no frontend pointer, C++ allocator, or LLVM type crosses the C ABI.

For a worked reading path, trace a call from [`checkExpression`](frontend/typechecking/expressions.cpp)
to [`checkCall`](frontend/typechecking/calls.cpp), then [IR call lowering](ir/lowering/calls.cpp)
and [native instruction emission](backend/native_functions.cpp). For the language rules themselves
use the [specification](../specs/README.md); for current coverage see the
[implemented surface](../docs/impl/supported-features.md).
