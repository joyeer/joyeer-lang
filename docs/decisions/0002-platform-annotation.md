# 0002: Platform-specific declarations (`@platform`)

- Status: accepted (implemented; language-designer and type-system reviews
  run twice and addressed; unfiltered CTest passes 1150/1150 on Windows x64 Debug;
  Release, other platforms, and `scripts/validate.py` were not run)
- Spec: [§11.5 `@platform`](../spec/11-annotations.md#115-platform)

## Context

Filesystem and other host code differs by operating system: path separators and
drive rules, native error codes, and API shapes. Joyeer has no way to select code by
target. Today there are two workarounds:

- Library code takes an explicit flag, as in
  [path.joyeer](../../lib/System/path.joyeer) (`windows: Bool`).
  Callers must supply it, and runtime wrappers have no way to detect the host.
- Anything platform-specific lives in C behind `#ifdef`, as in
  `filesystem.c` (since removed; the filesystem is now Joyeer, see [decision 0003](0003-pure-joyeer-filesystem.md)).

The [filesystem plan](../plan/filesystem.md) moves the filesystem into Joyeer
and needs a way to express the per-OS differences.

Constraints from the current compiler:

- Name resolution keys a scope's functions by name only
  ([nameresolution.cpp](../../compiler/frontend/nameresolution.cpp)). Two
  declarations of one name are `name-resolution.duplicate-declaration`.
- Annotations are already parsed generically, as a name plus arguments on
  `FunctionDeclSyntax`. The type checker rejects any annotation except
  `c_extern` and `c_export`.
- The frontend has no target. Only the backend knows one, and it always uses the
  host triple. The backend admits only x86-64/ARM64 Windows MSVC, Linux, and
  macOS.
- The compiler takes explicit source inputs and never discovers files
  ([library plan](../plan/library.md) §4).

## Options considered

1. **Function annotation, filtered before name resolution.**
   `@platform(windows)` and `@platform(posix)` on same-named functions; the
   compiler keeps the ones matching the target and discards the rest at the
   syntax level. Smallest change; reuses the annotation parser. Cannot swap a
   few lines or one import inside a function.
2. **Block-level conditional compilation** (Swift's `#if os(Windows)`). More
   flexible, but adds new syntax, lexer/parser rules, and a second place where
   declarations can disappear.
3. **File-level selection** (Go's build tags). Fern or the caller would pick
   per-platform source files. Needs no language feature, but puts the choice in
   manifest tooling, and tests that run in `validate` mode still need to name
   a target.
4. **Status quo with a host-detection primitive.** A scalar C function returns
   `Bool`; Joyeer code branches at run time. No compiler change, but every
   platform's code is compiled and type-checked everywhere, so Windows-only
   `@c_extern` bindings cannot exist in shared modules. The check also costs
   a run-time branch.
5. **Normalized C shim, no language feature** (as .NET's `SystemNative`). The C
   layer returns portable error enums and hides platform quirks, so Joyeer has
   few platform branches. This is orthogonal and does not cover lexical code
   such as drive-relative joins.

Options 4 and 5 can coexist with 1. They do not remove the need for a
compile-time selector if platform-specific Joyeer declarations are wanted.

## Decision

Option 1, with these rules (normative text: [§11.5](../spec/11-annotations.md#115-platform)).

- **Syntax.** `@platform(name, ...)` on a top-level function, with unlabelled
  identifier arguments: `windows`, `linux`, `macos`, and the group `posix`
  (every supported platform other than `windows`). The argument-clause parser
  already accepts unlabelled identifiers, so no parser change was needed; the
  grammar now states that each annotation fixes its own argument shape.
  Unlabelled arguments are the one exception to the labelled-argument style.
  They keep the common case short and were the spelling requested. A labelled
  form such as `os: windows` would leave room for a later `arch:` argument;
  that is the cost of this choice, and an architecture selector would need its
  own mechanism. Platform means operating system only.
- **Target.** `CompileOptions` gains a `targetPlatform`, defaulting to the
  host (`hostPlatform()`, chosen by the compiler's own build). The backend
  generates code only for the host, so any other target is rejected with
  `driver.unsupported-target-platform` unless the output mode is `validate`.
  There is no command-line option.
- **Selection.** A pass over the syntax tree runs before name resolution. A
  declaration whose platform list excludes the target is removed entirely;
  matching ones keep their body and lose the annotation, and their span then
  starts at the next remaining annotation or, failing that, the function name. Removed code leaves no trace in symbols or IR.
  A module with diagnostics is left unmodified.
- **Group rules**, checked on every host for the variants of one name:
  - Platform sets must not overlap; an unannotated function of the same name
    overlaps every variant.
  - Variants must have the same signature, compared as types after
    canonicalizing equivalent spellings (omitted result, `()` and `Void`;
    `[T]` and `Array<T>`; `T?` and `Optional<T>`; `[K: V]` and `Dict<K, V>`).
    Visibility is part of the signature. The comparison is on syntax, so a type
    that merely aliases another would compare unequal; the language has no
    aliases today.
  - A private function groups per file, as §12.2 scopes it, unless a
    non-private function of that name exists.
  - A variant name must not collide with a struct, enum, or top-level
    constant of the same scope, because those exist on every platform.
- **Missing variant.** Coverage is not required (a Windows-only function is
  legal). A reference to a name with no variant for the target is an ordinary
  unresolved-name error.
- **Companion annotations.** The parser admits a missing body after any
  annotation, and the type checker only inspects annotations that remain on a
  surviving function. The selection pass therefore checks, on every variant,
  that other annotations beside `@platform` are known and that a bodyless
  function carries `@c_extern`.
- **Inactive code is parsed but not type-checked.** Same trade-off as Rust's
  `#[cfg]`. Annotation shape on removed functions is still checked; bodies are
  not. The per-platform native validation gates remain the check for bodies.
- **Composition.** `@platform` combines with `@c_extern` and `@c_export` in
  either order. Only the selected variant's C annotation is checked, so
  disjoint variants may name the same C symbol. Native-binding authorization is
  unchanged and still comes from the toolchain context.
- **Scope.** Top-level functions only; a misplaced annotation on a method,
  type, field, or constant is a single `parser.unsupported-syntax` error.
  Imports are resolved before selection and cannot be conditional.

## Consequences

- Easier: portable runtime and library code in Joyeer with per-OS
  differences in one place; `path.joyeer` can drop its `windows:` parameter.
  Keep a pure function with the explicit flag beneath a thin `@platform`
  wrapper so lexical logic stays testable on every host.
- Harder: a second selection mechanism next to C `#ifdef`; code that never
  compiles on the current host; reading code requires knowing the target.
  A module's `public` interface depends on the platform it was compiled for.
- A new OS makes every `windows`/`posix` pair silently non-covering there; there
  is no `else` or `not`. `posix` is defined as "every non-Windows platform" so
  a new Unix-like target joins it.
- Tests: selection per target; overlap, signature, collision, and annotation
  rejection; equivalent spellings; private grouping; diagnostic order and
  spans; C binding combinations; service-level target guard, multi-file and
  dependency selection; an end-to-end fixture on the host platform. Debug-info
  absence of removed variants is not separately tested: removed functions never
  reach IR, which the LLVM text checks cover.
- New diagnostics with stable IDs: `platform.invalid-annotation`,
  `platform.unknown-name`, `platform.overlapping-variants`,
  `platform.signature-mismatch`, `platform.name-collision`, and
  `driver.unsupported-target-platform`. The pass runs before the type checker,
  so malformed arguments do not use `type-checking.invalid-annotation`.
- Pipeline impact: driver options, a new pre-resolution pass in the front end,
  a parser rule for misplaced annotations, a shared target definition, the
  spec, `supported-features.md`, and the skill reference. The type checker is
  unchanged: the annotation is stripped, not allowed.

## Follow-ups

- A note on the undefined-name error listing the platforms where the name is
  declared. It needs the pass to hand removed names to name resolution.
- A validate-only way to check every target from one machine (a command-line
  target option or an all-targets mode), so removed bodies can be checked
  without three hosts.
- Default argument values must join signature identity when they are added.
- If block-level `#if` is added later, decide whether `@platform` stays.
- Cached or precompiled library artifacts would need the platform in their key.

## Evidence

Prior art: Rust `#[cfg(...)]` on items; Go `//go:build` tags and `_windows.go`
suffixes; Swift `#if os(...)` blocks, with `@available` as a caller-side check
rather than a selector; C# `[SupportedOSPlatform]` as an analyzer only; Zig
`comptime` switch on the target. The thin-shim pattern appears in Java's native
dispatcher, libuv, and .NET `SystemNative`. These are from memory, not
rechecked against each project's source.

Reviews: the language-designer and type-system-reviewer roles ran after the
first implementation and found spelling-sensitive signature identity, a
module-wide grouping that disagreed with file-private scope, an unguarded
non-host target, unchecked annotations on removed functions, and misleading
member-placement diagnostics. This revision addresses them.
