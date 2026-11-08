# Examples

Larger Joyeer programs than the single-file samples in
[`skills/joyeer/examples/`](../skills/joyeer/examples/README.md). Each example
is its own fern project with a `project.toml`, and none is a member of the root
workspace, so building the compiler and running CTest never depends on them.

| Example | What it shows | Status |
|---|---|---|
| [`markdown-server/`](markdown-server/README.md) | A web server that serves a directory of Markdown files | Runs on Windows and Linux over the provisional TCP API ([§18.6](../docs/spec/18-host.md#186-tcp-networking)); macOS needs its TCP implementation |
