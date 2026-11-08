# Markdown web server

A static web server, written in Joyeer, that serves a directory of Markdown
files as a website. Each request reads the file, renders it to HTML, and
answers; there is nothing to build when a page changes.

> **Status: written ahead of its platform API.** The server uses the proposed
> TCP functions of `System` ([spec §18.6](../../docs/spec/18-host.md#186-tcp-networking),
> [decision 0012](../../docs/decisions/0012-tcp-networking.md)). They are not
> implemented yet, so `fern build` fails with unresolved `tcpListen`,
> `tcpAccept` and the rest until they are. The example is deliberately **not** a
> member of the root workspace, so it does not affect `fern check --workspace`
> or CTest.
>
> Everything that does not touch a socket (the Markdown renderer, request
> parsing, URL-to-file mapping, response building) was compiled and checked with
> the in-tree compiler against a stub of the network API. The socket calls were
> only type-checked, never run.

## Layout

| Path | Purpose |
|---|---|
| [`src/main.joyeer`](src/main.joyeer) | Arguments, accept loop, one request/response per connection |
| [`src/http.joyeer`](src/http.joyeer) | Reads the request head, parses the request line, builds responses |
| [`src/site.joyeer`](src/site.joyeer) | Maps a URL to a `.md` file safely; wraps pages in HTML |
| [`src/markdown.joyeer`](src/markdown.joyeer) | Markdown subset to escaped HTML |
| [`site/`](site/) | A sample site to serve |

Only `main.joyeer` and `http.joyeer` (reading the request head) call the
network API.

## Run it (once the network API exists)

From this directory:

```text
fern run --compiler <path-to-joyeer> -- site --port 8080
```

Then open <http://127.0.0.1:8080/>. Options: `[site-directory]`,
`--address a.b.c.d` (default `127.0.0.1`), `--port n` (default `8080`, `0` picks
a free port and prints it).

## URL mapping

| Request | File served |
|---|---|
| `/` | `site/index.md` |
| `/about` | `site/about.md`, else `site/about/index.md` |
| `/docs/` | `site/docs/index.md` |
| `/docs/markdown`, `/docs/markdown.md` | `site/docs/markdown.md` |

Anything else is a 404: only `GET` and `HEAD` are accepted (others get 405),
and the server never serves a file that is not a `.md` regular file.

## Safety properties

- **Path traversal.** A target is served only if every segment is made of ASCII
  letters, digits, `.`, `_` and `-` and does not start with `.`. That rejects
  `..`, hidden files, `:` (drive letters, NTFS streams), backslashes, percent
  escapes and non-ASCII names, before any file is touched.
- **Links.** The server refuses anything that is not a regular file, but it
  does not resolve symbolic links itself, and `fileKind` is not a sandbox.
  Keep the site directory free of links that point outside it.
- **Markup injection.** All text is HTML-escaped and links with schemes other
  than `http`, `https` and `mailto` are shown as text. Raw HTML in a page is
  displayed, never executed.
- **Memory.** A request head is limited to 16 KiB and a page to 1 MiB. Page
  authors are trusted: pathological Markdown (huge runs of unmatched `*`) can
  make rendering slow.

## Limits to know about

- **One connection at a time.** `System`'s sockets are blocking and the language
  has no threads yet, so a client that connects and stays silent blocks every
  other client. Do not expose this directly to the internet; put it behind a
  reverse proxy that enforces timeouts and terminates TLS.
- **No TLS**, no keep-alive, no caching headers, no request bodies.
- **Not CommonMark.** See [`site/docs/markdown.md`](site/docs/markdown.md).
- It binds `127.0.0.1` unless told otherwise. `--address 0.0.0.0` serves the
  whole network.
- Strings are built with `+`, so very large pages cost quadratic time. That is
  fine for a documentation site and a good first thing to improve.
