# Joyeer Documentation Index

> Joyeer is an **AI-era systems language**: AI writes most code, humans review
> and assist. Goal: replace C++ for new code. **No GC. Zero-cost abstractions.
> Swift-like syntax. Initially no `class` — use `struct`.**
>
> See the repository [AGENTS.md](../AGENTS.md) for build, test, and contribution rules.

---

## Where to Start

| You want to… | Read this |
|---|---|
| Understand **why** Joyeer exists | [design/ai-era-design.md](design/ai-era-design.md) |
| Know **what's shipping next** | [plan/v0.1.md](plan/v0.1.md) |
| Add or change a **language feature** | [design/](design/) — read the relevant doc, then update [plan/v0.1.md](plan/v0.1.md) |
| Add or change a **compiler / VM internal** | [impl/](impl/) + [plan/roadmap.md](plan/roadmap.md) |

---

## `design/` — What Joyeer Is (Long-term Commitments)

The semantics and guarantees the language promises. Changes here are major
decisions; update them rarely and deliberately.

| Doc | Topic |
|---|---|
| [ai-era-design.md](design/ai-era-design.md) | Philosophy: strong types, contracts, effect system as AI guardrails |
| [memory.md](design/memory.md) | Memory model: value semantics, RAII, no GC, no ARC |
| [parameter-passing.md](design/parameter-passing.md) | Call conventions: `let` / `inout` / `sink` (Hylo-style MVS) |
| [runtime-overhead.md](design/runtime-overhead.md) | Zero-cost contract: runtime size budget, no-hidden-work rules, ABI |
| [optional.md](design/optional.md) | `Option<T>` design |
| [grammar.md](design/grammar.md) | Surface syntax grammar |

---

## `impl/` — Current Implementation Details

How the **current C++ + VM** implementation works. Bound to the lifetime of
the current backend; will be revised or archived when LLVM lands.

| Doc | Topic |
|---|---|
| [bytecode.md](impl/bytecode.md) | VM bytecode opcode reference |
| [string.md](impl/string.md) | `String` runtime layout and builtin methods |

---

## `plan/` — Roadmap and Active Work

Short-lived planning documents. Updated frequently; outdated entries should be
deleted, not preserved.

| Doc | Topic |
|---|---|
| [roadmap.md](plan/roadmap.md) | Multi-phase pipeline plan (currently between Phase 1 and 2) |
| [v0.1.md](plan/v0.1.md) | v0.1 goal, feature checklist, blockers, gaps |

---

## Cross-Doc Conventions

- **One topic per doc.** If a feature needs splitting, link rather than duplicate.
- **Resolve contradictions immediately.** If two docs disagree on a committed
  decision, fix the older one — design docs are a single source of truth.
- **Plan docs may go stale; design docs may not.** Treat any drift between
  `design/` files as a bug.
- **No year stamps in titles.** Docs are dated by git history.
