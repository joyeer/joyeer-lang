# Examples

Larger Joyeer programs than the single-file samples in
[`skills/joyeer/examples/`](../skills/joyeer/examples/README.md). Each example
is its own fern project with a `project.toml`, and none is a member of the root
workspace, so building the compiler and running CTest never depends on them.

| Example | What it shows | Status |
|---|---|---|
| [`web-server/`](web-server/README.md) | A web server that serves a directory of Markdown files, with a pool of worker tasks fed through channels | Uses the provisional [TCP API](../specs/18-host.md#186-tcp-networking); verified on Windows and Linux, with macOS validation pending |
