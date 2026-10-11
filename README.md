# Joyeer

Joyeer is an **AI-era systems programming language** for native software.
It combines Swift-like syntax, strong typing, and value semantics with
deterministic resource cleanup and no garbage collector. It is designed for
a workflow where AI writes code and humans review and refine it.

- **Readable code:** explicit declarations and named arguments make intent
  easy to follow.
- **Predictable resource lifetimes:** ownership and value types keep resource
  management visible.
- **Explicit error handling:** use `Optional` for absence and `Result` for
  recoverable failures.
- **Native executables:** compile programs for the host platform.

## Getting started

### Install locally

Follow the [source-build guide](docs/building.md) to build Joyeer, then run the
[local installer](docs/building.md#local-debug-installation) from this checkout.

Windows:

```powershell
python .\scripts\install-debug.py
```

macOS and Linux:

```bash
python3 scripts/install-debug.py
```

The installer uses an existing Debug build and requires Python 3.9+ and CMake.
It installs the compiler, joypm when enabled, and the Joyeer agent skill, and
adds the tools to PATH. **Open a new terminal or restart your IDE afterward.**
You can then invoke `joyeer` and `joypm` directly; no build-directory paths
are needed.

Using the installed compiler or running compiled programs does not require
Python or an external LLVM installation. Native linking still needs the
platform SDK and system libraries, including MSVC Build Tools and a Windows
SDK on Windows.

### Write your first program

Save this as `hello.joyeer`:

```joyeer
func add(left: Int, right: Int): Int {
    return left + right
}

func main() {
    let answer = add(left: 20, right: 22)
    print(value: answer)
}
```

Check the program without running it:

```text
joyeer hello.joyeer
```

Build and run on Windows:

```powershell
joyeer -o hello.exe hello.joyeer
if ($LASTEXITCODE -ne 0) { throw 'Compilation failed' }
.\hello.exe
```

On macOS and Linux:

```bash
joyeer -o hello hello.joyeer && ./hello
```

The program prints `42`.

### Useful compiler options

| Command | Purpose |
|---|---|
| `joyeer -O0 -gfull -o hello.exe hello.joyeer` | Build with debug information |
| `joyeer -O2 -o hello.exe hello.joyeer` | Build an optimized executable |
| `joyeer --emit-llvm hello.ll hello.joyeer` | Write LLVM IR for inspection |

Use `hello` instead of `hello.exe` on macOS and Linux. See the
[compiler command reference](docs/impl/backend.md#2-cli) for more options.

## Local projects with joypm

Use `joypm` to create, check, build, run, and test projects. Each `joyeer.toml`
defines one module and owns its sources and dependencies.

PowerShell example, using a new `hello-project` directory:

```powershell
joypm init hello-project --name hello.app
Set-Location hello-project
$compiler = (Get-Command joyeer -CommandType Application -ErrorAction Stop).Source
joypm check --compiler $compiler
joypm run --compiler $compiler
```

joypm takes an explicit compiler path. The example resolves the installed
`joyeer` from PATH once, so no machine-specific path is needed. In Bash or
Zsh, use `--compiler "$(command -v joyeer)"`.

From the module directory:

| Command | Purpose |
|---|---|
| `joypm build --compiler $compiler --release` | Build an optimized executable |
| `joypm test --compiler $compiler` | Build and run test modules |

For multiple modules, list their directories in `workspace.toml` and pass
`--workspace workspace.toml`. Add `--module <name>` to select one member.
See the [module and workspace guide](docs/impl/joypm.md) for manifest examples,
test setup, and output locations.

## AI-assisted development

The [Joyeer agent skill](skills/README.md) provides language guidance, command
examples, and ownership/error-handling conventions for VS Code Copilot,
Copilot CLI, Claude Code, and Codex. The local installer includes it by
default. Restart your agent session after installation; see the skill guide
for discovery and agent-specific setup.

## Documentation

| Topic | Guide |
|---|---|
| Language reference | [Joyeer specification](docs/spec.md) |
| Design philosophy | [Why Joyeer](docs/spec/00-preamble.md) |
| Memory and ownership | [Memory model](docs/spec/04-memory.md) |
| Build, install, and troubleshoot | [Building Joyeer](docs/building.md) |
| All documentation | [Documentation index](docs/README.md) |

## Contributing

For compiler development, build/test instructions are in the
[building guide](docs/building.md) and development conventions are in
[AGENTS.md](AGENTS.md).

## License

Licensed under the [Apache License 2.0](LICENSE).