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
| Know **what the language is** (normative) | [spec.md](spec.md) |
| Understand **why** Joyeer exists | [rationale/ai-era-design.md](rationale/ai-era-design.md) |
| Know **what's shipping next** | [plan/v0.1.md](plan/v0.1.md) |
| Add or change a **language feature** | edit [spec.md](spec.md); record the *why* in [rationale/](rationale/) |
| Add or change a **compiler / VM internal** | [impl/](impl/) + [plan/roadmap.md](plan/roadmap.md) |

---

## `spec.md` — The Normative Specification

The single source of truth for **what the language is**: lexical structure,
types, declarations, memory model, expressions, statements, patterns,
contracts, effects, modules, grammar. All other docs defer to it.

| Doc | Topic |
|---|---|
| [spec.md](spec.md) | Full language specification (§1–§17) |

---

## `rationale/` — Why Joyeer Is the Way It Is

The reasoning behind the spec: design surveys, trade-offs, and rejected
alternatives. These explain *why*; the spec defines *what*. Where the two
disagree, the spec wins.

| Doc | Topic |
|---|---|
| [ai-era-design.md](rationale/ai-era-design.md) | Philosophy: strong types, contracts, effect system as AI guardrails |
| [memory.md](rationale/memory.md) | Memory model rationale: value semantics, RAII, no GC, no ARC (normative: spec §4) |
| [parameter-passing.md](rationale/parameter-passing.md) | Call-convention rationale: Hylo-style MVS (normative: spec §4.2–§4.3) |
| [runtime-overhead.md](rationale/runtime-overhead.md) | Zero-cost contract: runtime size budget, no-hidden-work rules, ABI |

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
| [spec-impl-plan.md](plan/spec-impl-plan.md) | Phased plan for implementing the spec in the compiler/VM |
| [feasibility.md](plan/feasibility.md) | Can the design be built? Per-feature prior art, combination risks, layered-verification ceiling |

---

## Cross-Doc Conventions

- **One topic per doc.** If a feature needs splitting, link rather than duplicate.
- **The spec is the single source of truth.** If a `rationale/` or `plan/` doc
  disagrees with [spec.md](spec.md) on a committed decision, the spec wins; fix
  the other doc.
- **Plan docs may go stale; the spec and rationale may not.** Treat any drift
  between the spec and a `rationale/` file as a bug.
- **No year stamps in titles.** Docs are dated by git history.
