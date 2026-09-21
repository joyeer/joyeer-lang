# Runnable examples

Compile each `.joyeer` file separately. These examples are also registered as
native CTest cases in the source repository, so the distributed sources are
compiled and their exact output checked rather than maintained as pseudocode.

| Source | Expected stdout, one item per line |
|---|---|
| [hello.joyeer](hello.joyeer) | `Hello, Joyeer!`, `42` |
| [collections.joyeer](collections.joyeer) | `2`, `42`, `Joy`, `66`, `7` |
| [ownership-and-errors.joyeer](ownership-and-errors.joyeer) | `42`, `hello!`, `reinitialized`, `7`, `absent`, `8`, `expected non-negative` |
| [read-file.joyeer](read-file.joyeer) | `2` when run from this directory with the bundled [input.txt](input.txt) |

Each successful output ends with a newline. The file-input example counts LF
bytes, so it works with either LF or CRLF line endings in the bundled input.
In an empty working directory without `input.txt`, it prints `read failed`.
That message is an explicitly handled error branch, not a successful file read.

From this directory, in PowerShell, using an existing scratch output directory:

```powershell
joyeer -o "$env:TEMP\joyeer-skill-hello.exe" hello.joyeer
if ($LASTEXITCODE -ne 0) { throw "Joyeer compilation failed" }
& "$env:TEMP\joyeer-skill-hello.exe"
if ($LASTEXITCODE -ne 0) { throw "Joyeer program failed" }
```

Choose an unused output name and clean up only artifacts you created. See
[CLI usage](../references/cli.md) for other hosts and known Windows path limits.
