# Joyeer agent skill

[joyeer/SKILL.md](joyeer/SKILL.md) is the portable, user-facing skill for writing
Joyeer programs with VS Code Copilot, Copilot CLI, Claude Code, or Codex. It uses common Agent
Skills metadata (`name` and `description`), relative references, and executable
examples. It does not require Python, an MCP server, agent-specific tools,
permission overrides, or internet access.

This is not a compiler-contributor skill. The repository's
[AGENTS.md](../AGENTS.md) describes the C++ compiler development workflow.

## Install for a local agent

On Windows, the [Debug installer](../scripts/install-debug.ps1) installs the
compiler and this complete skill together by default:

```powershell
.\scripts\install-debug.ps1
```

The skill goes to `$HOME\.agents\skills\joyeer`. Use `-SkillDir` to choose a
different full destination, for example `-SkillDir "$HOME\.claude\skills\joyeer"`
for Claude Code, or `-SkipSkillInstall` to install only the compiler. Only one
skill destination is installed per run. Identical installed files are kept;
new and empty destinations accept a full installation. Missing or differing
files in a nonempty destination cause an error before compiler installation.
After reviewing and backing up local skill edits, use
`.\scripts\install-debug.ps1 -Force` to overwrite differing files and restore
missing files from the current checkout. Additional files (including obsolete
files no longer in the source) and other skills are not deleted. File/directory
type conflicts still require manual resolution, and `-SkipSkillInstall` takes
precedence over `-Force`.

For manual installation, copy the **whole `joyeer` directory**, including references and examples, into
one of the locations below. These are defaults for current skill-capable
clients, not a promise about older versions or every configuration.

| Agent | Personal destination (Windows) | Project destination |
|---|---|---|
| VS Code Copilot, Copilot CLI, and Codex | `%USERPROFILE%\.agents\skills\joyeer` | `.agents\skills\joyeer` |
| Claude Code | `%USERPROFILE%\.claude\skills\joyeer` | `.claude\skills\joyeer` |

Copilot CLI also supports `.copilot\skills` in the user's home directory and
`.github\skills` in a repository. Choose one location for a given scope, rather
than installing the same skill in every directory an agent scans. Copilot and
Codex can share the personal `.agents` copy; Claude Code uses a separate copy
of the same source. Resolve the actual home/configuration paths for the agent.
On Linux/macOS, use the equivalent directories under the agent user's home.

Example from the repository root in PowerShell, for Copilot CLI and Codex:

```powershell
$parent = Join-Path $HOME ".agents\skills"
$destination = Join-Path $parent "joyeer"
if (Test-Path -LiteralPath $destination) {
    throw "A Joyeer skill already exists; review it before replacing it."
}
New-Item -ItemType Directory -Path $parent -Force | Out-Null
Copy-Item -LiteralPath ".\skills\joyeer" -Destination $destination -Recurse
```

For Claude Code, use `.claude\skills` as the parent instead. Building this
repository or running CMake install alone does not deploy skills; the Debug
installation script and the manual copy above do.

Restart the agent session after installation. In VS Code, use `/skills` to check
discovery and `/joyeer` to invoke the skill. Copilot CLI also documents
`/skills reload` and `/skills info joyeer`. Claude Code supports `/joyeer`;
in Codex, select the installed skill through its skills interface or name
`$joyeer` in the prompt. Confirm discovery in the actual client before relying
on automatic invocation from the description.

Local skill files do not install the compiler or synchronize into WSL,
containers, remote hosts, or cloud sessions. Install in each intended execution
environment and make the matching compiler available there.

Official client documentation:

- [VS Code Copilot skills](https://code.visualstudio.com/docs/copilot/customization/agent-skills)
- [Copilot CLI skills](https://docs.github.com/en/copilot/how-tos/copilot-cli/customize-copilot/add-skills)
- [Claude Code skills](https://code.claude.com/docs/en/skills)
- [Codex skills](https://developers.openai.com/codex/skills)

## Maintain and distribute

- Treat `skills\joyeer` as the only authored product-skill source. Do not
  maintain independent Copilot/Claude/Codex language guides.
- Keep the entry point short; update references against the implemented
  compiler, not merely future specification syntax. The normative design is
  [docs/spec.md](../docs/spec.md); current scope is
  [docs/plan/v0.1.md](../docs/plan/v0.1.md).
- Update the compatibility baseline when the compiler changes. Publish the
  skill with its matching release/revision rather than linking to moving
  development documentation.
- The [local Debug installer](../docs/building.md#local-windows-debug-installation)
  deploys the skill alongside compiler installation unless `-SkipSkillInstall`
  is supplied. The skill is still not part of the CMake runtime-install payload.
- Review existing destinations before upgrading. Preserve user edits and other
  skills; an uninstaller must remove only files it owns and must not delete an
  entire shared agent directory.

The bundle uses the repository's [license](../LICENSE); distributors must
include the applicable license material with the product.

## Verification

Reconfigure after adding examples, build, and run unfiltered CTest for the
repository acceptance gate. For focused iteration, the `skill` label compiles
and runs every bundled program, checks its exact stdout, and exercises the
file-input failure branch with an isolated working directory:

```text
ctest --test-dir out\build\arm64-debug -L skill --output-on-failure
```

Use the build directory for the actual host/preset. When changing examples,
update their expected output and registration in `tests\CMakeLists.txt`.
Compiler checks do not establish that every agent has discovered the skill;
client integration must be checked separately in the intended client/version.
