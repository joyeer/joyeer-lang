# Markdown web server

A static web server, written in Joyeer, that serves a directory of Markdown
files as a website. Each request reads the file, renders it to HTML, and
answers; there is nothing to build when a page changes. A pool of worker tasks
serves connections concurrently ([spec §21](../../specs/21-concurrency.md)).

> The server uses the provisional [TCP functions of `System`](../../specs/18-host.md#186-tcp-networking).
> `GET`, `HEAD`, `404`, and `405` were checked with the worker pool on Windows
> x64 and Linux x86-64 (WSL2). macOS TCP is implemented, but no native macOS
> server run is recorded. The example is not in the root Fern workspace;
> CTest exercises its request-head deadline and idle-worker dispatch.

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
- **Request timeout.** The server has five seconds to read the entire request head once a
  worker starts reading it. Partial reads do not reset the timer. If the deadline expires
  before the head is complete, the server replies with `408 Request Timeout`.
- **Memory.** A request head is limited to 16 KiB and a page to 1 MiB. Page
  authors are trusted: pathological Markdown (huge runs of unmatched `*`) can
  make rendering slow.

## How the pool works

Each worker has two capacity-one channels: a job inbox and a signal that it is idle. The accept
loop waits for any signal, receives it, then **moves** the accepted connection to that worker.
If all workers are busy, the accept loop holds one connection until one becomes idle. It never
queues a job behind a busy worker while another is idle. Nothing is shared, and pages are read
and rendered per request without a shared cache.

On shutdown, the accept loop closes the job senders and idle receivers before joining so a
worker can leave either blocking channel operation ([decision 0019](../../docs/decisions/0019-channels.md)).

## Measured behavior

These historical measurements used round-robin dispatch, not the current idle-worker selection.
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

## Limits to know about

- **A stalled connection still occupies its worker.** The request head has a five-second total
  budget, but the example's response send is still unbounded. If every worker is occupied,
  dispatch waits.
  Do not expose this directly to the internet; put it behind a reverse proxy that enforces timeouts and terminates
  TLS.
- **`runProcess` cannot be used** in a program that starts tasks ([spec §21.6](../../specs/21-concurrency.md)).
- **No TLS**, no keep-alive, no caching headers, no request bodies.
- **Not CommonMark.** See [`site/docs/markdown.md`](site/docs/markdown.md).
- It binds `127.0.0.1` unless told otherwise. `--address 0.0.0.0` serves the
  whole network.
- Strings are built with `+`, so very large pages cost quadratic time. That is
  fine for a documentation site and a good first thing to improve.
