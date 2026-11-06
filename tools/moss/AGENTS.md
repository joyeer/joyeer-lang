# AGENTS.md - Moss

Moss is an AI-oriented reverse-engineering CLI written in Joyeer and built with
fern. Today it decodes JVM `.class` files (standalone or inside a ZIP/JAR) and
prints exact bytecode; Java source decompilation is a later phase. Read
[README.md](README.md) first, then [docs/jvm.md](docs/jvm.md) (CLI and output
contract) and [docs/plan.md](docs/plan.md) (delivery plan and open gates).

Write all documentation in English.

## Build and test

Moss is built from the repository root; see the README for the commands.

```text
cmake -S . -B build -G Ninja
cmake --build build --target moss
ctest --test-dir build -L moss --output-on-failure
```

- ZIP support uses the toolchain's standard `Compression` module
  (`JOYEER_BUILD_COMPRESSION`, on by default), resolved by the compiler from `lib/`. Do not
  vendor or stage a copy.
- Optional JDK gates (need an explicit JDK, never a dependency of the product):
  `integration.cjs <moss> --javap <javap>` and
  `tests/jvm/corpus.cjs <moss> <javap> <class-root>` over real javac classes.
- For code or behavior changes, run Debug and Release before claiming
  acceptance. Documentation-only edits follow the
  [root verification policy](../../AGENTS.md#verification-workflow).
  Do not call a gate complete that was not executed.

## Layout and rules

- One module per directory with its own `project.toml`; list every member in
  [workspace.toml](workspace.toml). Do not add placeholder modules.
- `moss.binary` (checked readers, input), `moss.model` (facts, IDs,
  diagnostics), `moss.jvm` (classfile and decoder), `moss.archive` (ZIP),
  `moss.output` (rendering), `moss.cli` (options, queries), `moss.app`
  (process boundary). Keep parsing, decoding, analysis, and presentation
  separate; `moss.output` must not import the decoder.
- Never trap on malformed input: check bounds against remaining length before
  indexing or adding. Report failures as `Diagnostic` values with stable codes.
- Use explicit imports and public short names; mark read-only non-scalar
  parameters `borrowing`.
- Treat the checked-in `.class` fixtures and `tests/fixtures/jvm/Fixtures.jar` as
  durable data; do not regenerate them casually.
- Follow the root [.gitattributes](../../.gitattributes): text files use LF
  on every platform; preserve byte-exact binary fixtures.
- Output is plain text, bounded (8 MiB), and follows `javap -c -p -l` of JDK 25
  byte for byte; there is no JSON or NDJSON. Changing the layout is a contract
  change: update [docs/jvm.md](docs/jvm.md), the `*.javap.txt` goldens, the
  tests, and `skills/moss/SKILL.md` together. Compare against a real javap
  (`integration.cjs --javap`, `corpus.cjs`) rather than writing expectations by hand.

## Agent integration

[skills/moss/SKILL.md](skills/moss/SKILL.md) is the single source for agent
guidance. [scripts/install.py](scripts/install.py) copies the binary into a skill
folder and can generate an AGENTS.md / Copilot-instructions section from it.
Keep SKILL.md accurate whenever commands or limits change.

## Session mode

`moss serve` ([docs/jvm.md](docs/jvm.md#persistent-session-moss-serve)) is a plain-text
line protocol, not MCP and not JSON. Its request tokenizer, reply framing, and
cache live in `src/cli/serve.joyeer` (`moss.cli`); `src/main.joyeer` only owns the
stdin loop. Replies count lines, not bytes, because Windows stdout converts LF to
CRLF. Keep the cache observable (`Session.reads`/`parses`) and covered by
`tests/main.joyeer`, and keep `Moss.Serve` (a real process fed from `tests/serve/`)
passing. Changing the request or reply format is a contract change: update
docs/jvm.md, the tests, and `skills/moss/SKILL.md` together.
