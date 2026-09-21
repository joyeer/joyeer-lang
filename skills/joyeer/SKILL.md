---
name: joyeer
description: Write, compile, run, and debug Joyeer programs. Use when a task involves .joyeer source files, Joyeer syntax, ownership, built-in containers, or compiler diagnostics. This skill is for using the language, not implementing its C++ compiler.
---

# Joyeer programming

Use the Joyeer compiler as the feedback loop. Do not infer language support
from resemblance to Swift, Rust, Go, or C++. This skill describes the current
single-file native implementation, not every feature in the draft specification.

## Before writing code

1. Read [supported features](references/supported-features.md) and
   [CLI usage](references/cli.md). Check the selected compiler's provenance
   against the skill's source or release. This baseline does **not** implement
   `--version` or `--help`; do not invent a version check.
2. Read the relevant parts of the
   [language guide](references/language-guide.md). All reference paths are
   relative to this skill directory, not the user's working directory.
3. Locate the installed `joyeer` executable, or use the project's explicitly
   configured compiler path. If unavailable, report that prerequisite; do not
   download tools, modify PATH, or build the compiler without authorization.
4. Identify unsupported requirements before coding. Explain the limitation and
   use a supported alternative where it preserves the requested behavior.
   Do not silently replace the requested implementation with a different language.

## Write the program

- Prefer `let`; use `var` for mutation. Put executable work in functions and
  use a parameterless `func main()` for native programs.
- Write explicit parameter and result types. Use named calls such as
  `print(value: result)`; function result types follow `:`, not `->`.
- Respect borrowing, `inout`, consuming, and initializing access.
  Do not remove required `&` or `consume` markers merely to hide a diagnostic.
- Use exhaustive `match` for enums, `Optional`, and `Result`. Handle failure
  explicitly rather than replacing it with an apparent success.
- Keep compilation to one source file. Do not invent imports, external
  packages, standard-library methods, or project-manager commands.
- Consult the [tested examples](examples/README.md) for concrete spellings.
  Adapt them to the task; do not treat them as a complete standard library.

## Compile, run, and repair

1. Validate the source with `joyeer source.joyeer`. This checks and lowers the
   program but does not execute it.
2. Build with `joyeer -o output.exe source.joyeer` on Windows, or choose a
   suitable native output name on the host. Use a dedicated output directory;
   never overwrite the source or unrelated files.
3. Run the output only after successful compilation, using the required working
   directory and inputs. Check exit status and actual output against the task,
   including failure and boundary cases where applicable.
4. Read the compiler's complete diagnostic, including stable ID, location,
   notes, and help. Fix the cause, then compile and run again.
5. Distinguish language errors from missing platform SDKs and other environment
   failures. Report blocked execution accurately; frontend validation or emitted
   LLVM text alone is not proof that a native program works.

Use the agent's existing file and process tools with their normal permission
checks. No product-specific tool names, hooks, elevated permissions, network
access, or helper scripts are required by this skill.
