# Markdown web server

A static web server, written in Joyeer, that serves a directory of Markdown
files as a website. Each request reads the file, renders it to HTML, and
answers; there is nothing to build when a page changes. A pool of worker tasks
serves connections concurrently ([spec §21](../../specs/21-concurrency.md)).

> **Status: runs on Windows.** The server uses the provisional TCP functions of
> `System` ([spec §18.6](../../specs/18-host.md#186-tcp-networking),
> [decision 0012](../../docs/decisions/0012-tcp-networking.md)), which are
> implemented for Windows and Linux; on macOS they fail with `.Other(38)`, so
> the server prints a listen error and exits. It was built with the in-tree
> compiler and checked over real HTTP on Windows x64 and on Linux x86-64
> (Ubuntu under WSL2), both Debug (`GET`, `HEAD`, `404`, `405`), including the
> worker pool described below. The example is deliberately **not** a member of the root
> workspace, so it does not affect `fern check --workspace` or CTest.

## Layout

| Path | Purpose |
|---|---|
| [`src/main.joyeer`](src/main.joyeer) | Arguments, the accept loop that deals connections to workers, the worker task |
| [`src/http.joyeer`](src/http.joyeer) | Reads the request head, parses the request line, builds responses |
| [`src/site.joyeer`](src/site.joyeer) | Maps a URL to a `.md` file safely; wraps pages in HTML |
| [`src/markdown.joyeer`](src/markdown.joyeer) | Markdown subset to escaped HTML |
| [`site/`](site/) | A sample site to serve |

Only `main.joyeer` and `http.joyeer` (reading the request head) call the
network API.

## Run it

From this directory (the compiler must be built with the network library
staged beside it):

```text
fern run --compiler <path-to-joyeer> -- site --port 8080
```

Then open <http://127.0.0.1:8080/>. Options: `[site-directory]`,
`--address a.b.c.d` (default `127.0.0.1`), `--port n` (default `8080`, `0` picks
a free port and prints it), `--workers n` (default `4`, from 1 to 64).

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

## How the pool works

The accept loop makes one bounded channel per worker (capacity 4) and starts one task per worker, giving each
task the receiving end ([decision 0019](../../docs/decisions/0019-channels.md)). It then deals each accepted
`TcpConnection` to the next worker in turn with `send`; the connection is **moved** into the queue, so exactly one
task owns it at any time: the accept loop until it sends, then the worker, which serves it, closes it and waits for
the next one. Nothing is shared and there is no lock in the program. A page is read and rendered per request;
there is no shared cache, by design, because a cache would need shared mutable state.

When accepting keeps failing, the accept loop closes every queue (a worker finishes what it holds, sees its queue
closed and returns), joins the workers, and exits. The senders are closed before the joins on purpose: a worker
ends only when its sender is gone, so joining first would wait forever ([spec §21.9.5](../../specs/21-concurrency.md)).

## Measured behavior

One run each, on a 16-logical-CPU Windows 11 machine with Ubuntu under WSL2, with the **Debug** toolchain and
runtime (so absolute numbers are far from what an optimized build would do), driven by `curl` (whose process
start-up dominates the small pages). These are indications, not a benchmark:

| Server | 600 requests, 16 at a time | One request while another client stays silent |
|---|---|---|
| one connection at a time (before) | 2.5 s | 4.7 s |
| pool, 1 worker | 2.8 s | 4.7 s |
| pool, 4 workers | 1.7 s | 0.005 s |
| pool, 8 workers | 2.3 s | 0.005 s |

The pool serves in parallel, and a silent client no longer stops everyone. The last column hides a cost, shown
by sending eight requests, one after another, while one client holds a connection silent:

```text
single thread:  4.69  0.004 0.004 0.004 0.004 0.004 0.004 0.004   (seconds)
pool, 4:        0.006 0.005 0.005 4.63  0.005 0.005 0.005 0.005
pool, 8:        0.005 0.005 0.005 0.005 0.005 0.005 0.005 4.60
```

**Head-of-line blocking.** Connections are dealt round-robin without asking whether a worker is idle, so one in
every *N* requests lands on the worker that holds the silent client and waits for its 5-second read timeout, even
though the other workers are free. Channels have a single sender and no `select` or try-receive yet, so the accept
loop cannot learn which worker is idle ([decision 0013](../../docs/decisions/0013-concurrency-model.md), question 11).
More workers make a stall less likely, and the per-connection read timeout bounds it; neither removes it. The same
run on Windows shows the same shape (one of eight requests waits about 4.9 s).

## Limits to know about

- **A stalled connection stalls its worker and the queue behind it** (above). A client that connects and stays
  silent holds one worker for at most 5 seconds per read ([decision 0014](../../docs/decisions/0014-network-timeouts.md)),
  then gets `408 Request Timeout`. That is a bound, not a defense: a client that sends a byte every few seconds, or
  never reads the response, can still tie a worker up, because there is no total deadline and no send timeout.
  Do not expose this directly to the internet; put it behind a reverse proxy that enforces timeouts and terminates
  TLS.
- **`runProcess` cannot be used** in a program that starts tasks ([spec §21.6](../../specs/21-concurrency.md)).
- **No TLS**, no keep-alive, no caching headers, no request bodies.
- **Not CommonMark.** See [`site/docs/markdown.md`](site/docs/markdown.md).
- It binds `127.0.0.1` unless told otherwise. `--address 0.0.0.0` serves the
  whole network.
- Strings are built with `+`, so very large pages cost quadratic time. That is
  fine for a documentation site and a good first thing to improve.
