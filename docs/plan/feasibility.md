# Joyeer — Design Feasibility

> Can the Joyeer design actually be built? This doc separates two questions
> that are easy to conflate:
>
> 1. **Is the design theoretically/engineering-wise possible?** — mostly *yes*.
> 2. **Is the v0.1 implementation there yet?** — mostly *no* (see
>    [v0.1.md](v0.1.md) and [roadmap.md](roadmap.md)).
>
> This document answers **(1)**. It records the current conclusion so we can
> revisit and expand it later. Where it touches *what the language is*, the
> normative source remains [../spec.md](../spec.md); the *why* remains
> [../rationale/](../rationale/).

---

## TL;DR

- **The "engineering version" of Joyeer is clearly buildable.** Every feature
  Joyeer wants has already been proven — *individually* — in a shipping
  language. Nothing here requires a research breakthrough.
- **The risk is integration, ergonomics, and scope/time — not theoretical
  possibility.** It needs an experienced team and years of polish, not a Nobel
  prize.
- **The one genuinely hard part — fully automatic formal verification of
  arbitrary properties — has a theoretical ceiling and should *not* be
  promised.** It is also *not necessary*: a layered fallback (runtime →
  decidable refinement → SMT-assisted) is already stronger than ~99% of
  production languages today.

---

## 1. Each feature is already proven — individually

Joyeer is not betting on an unsolved research problem; it is betting on
**engineering integration**. The table below maps each major design commitment
to a real language that already ships it.

| Feature | Prior art (proof it works) | Notes |
|---|---|---|
| No GC + value semantics + ownership | **Rust, Swift, Hylo, Mojo** | Hylo (ex-Val) is almost exactly Joyeer's "no reference types, mutable value semantics" target. The path has been walked. |
| `borrowing` / `inout` / `consuming` / `initializing` | **Swift 5.9+** (near-verbatim) | Joyeer borrows Swift's ownership keywords directly. Shipping ⇒ implementable. |
| 4 subscript accessors + `yield` | **Swift** `_read` / `_modify` coroutine accessors | Already runs in a production compiler. |
| Effect system (`performs IO`, …) | **Koka, Effekt, Unison, OCaml 5** | Active but mature area; effect handlers are real. |
| Contracts (`requires` / `ensures`) | **Eiffel, Ada/SPARK, Dafny, Verus** | Runtime contracts are 1986 tech; compile-time verification proven by SPARK / Dafny / Verus. |
| Refinement types (`PositiveInt`, `NonEmpty`) | **Liquid Haskell, F\***, Dafny | Decidable subsets are well understood. |
| Zero-cost generics (monomorphization) | **Rust, C++** | Standard practice. |
| LLVM backend | **Rust, Swift, Clang** | Large but fully known path. |

**Conclusion:** no single feature needs a new theory. Difficulty lives in
*combining* them and in *years of polish*, not in feasibility.

---

## 2. The combination risks (and why they are solvable)

Feasibility risk is in the *interactions*, not the parts. Three known tension
points, all with a viable path forward:

### 2.1 Ownership-checker ergonomics — *solvable, tunable*

The current memory model uses statement-granularity projection lifetimes with
no NLL-style refinement and index-level conservatism (spec §4.4.2, §4.5.3).
A conservative checker is the *easiest* to implement; the open question is
whether it is *pleasant enough* to use.

This is a **tunable parameter, not a yes/no problem**. Rust itself evolved from
"borrow checking is annoying" to NLL to Polonius. Joyeer can ship coarse and
refine later (add NLL, finer alias analysis). Recommended: **prototype a
minimal exclusivity checker early** and stress-test it on real code (JSON
parser, quicksort) before locking the spec.

### 2.2 Index-alias safety — *solvable, known answer*

Banning reference types (spec §4.8) pushes graph/cyclic structures onto
integer indices, where the Law of Exclusivity currently gives **zero**
guarantees (an index is just an `Int`: stale or cross-container indices are
unchecked use-after-free-class bugs).

This has a **mature solution**: generational indices / typed handles / arenas
(cf. Rust's `slotmap`, ECS `Entity`). A checked `Handle<T>` / arena abstraction
turns use-after-free into a detectable generation mismatch. So this is
**"not yet designed," not "cannot be designed."** Until it exists, the boundary
should be *documented honestly* rather than left implicit.

### 2.3 Compile-time verification ceiling — *partially solvable; the only true hard part*

Runtime contracts are 100% implementable (Eiffel did it decades ago). The
rationale's picture of "AI generates + verifier *proves* `ensures isSorted`"
runs into a real ceiling: **fully automatic** verification of arbitrary
properties is undecidable in general — SMT solvers time out, loop invariants
often need human hints, and properties like `isPermutation` may not be proven
automatically even by Dafny.

But the **fallback path is smooth**, and the layers can ship incrementally:

```
Layer 1: Runtime checks (precondition/assert)        ← shippable today; Eiffel-level
Layer 2: Compile-time decidable subset               ← refinement types;
         (non-null, range, exhaustiveness)              Liquid Haskell-level
Layer 3: SMT-assisted verification (simple ensures)  ← Dafny / Verus-level; semi-automatic
Layer 4: Fully automatic proof of arbitrary props    ← no language achieves this;
                                                        must NOT be promised
```

**Key insight:** Joyeer does not need Layer 4 to be valuable. Reaching
**Layer 2–3** already exceeds ~99% of production languages. The design is
implementable as long as the spec does **not** write "Layer 4, fully
automatic" as a release promise.

---

## 3. Verdict

- ✅ **As an integrated systems language** — "no GC + ownership + effects +
  runtime contracts + refinement types + LLVM backend" — **clearly buildable.**
  Each block has prior art; the cost is integration engineering and years of
  polish, not feasibility. This is "needs an experienced team for a long time,"
  not "needs a breakthrough."

- ⚠️ **As "AI generates, verifier proves arbitrary correctness"** — has a
  theoretical ceiling and will **never** be 100% automatic. It also doesn't
  need to be; landing at SMT-assisted (Layer 3) is implementable and already
  strong.

In short: the **engineering version is necessarily buildable**; the
**sci-fi version (fully automatic formal verification) is not — and need not
be.** Residual risk is entirely in execution (scope, team, time), **not** in
whether the design is physically/theoretically possible.

---

## 4. Recommended spec note (to de-risk the flagship claim)

Add a layered-verification statement to the rationale/spec so the "core
differentiator" stops being an open-ended promise and becomes a planned,
incremental feature:

> Contract semantics land in four layers (runtime → decidable refinement →
> SMT-assisted → fully automatic). Joyeer commits through **Layer 3**; Layer 4
> is a research direction, **not** a release promise.

With that change, the previously-noted "flagship feature is vaporware" gap
downgrades from a **contradiction** to a **known, planned, incremental
feature**.

---

## Open items (to revisit later)

- [ ] Prototype a minimal exclusivity checker; stress-test ergonomics (§2.1).
- [ ] Design the checked `Handle<T>` / arena abstraction for index-alias
      safety, or document the unsafe boundary explicitly (§2.2).
- [ ] Decide the layered-verification commitment and reflect it in
      [../rationale/ai-era-design.md](../rationale/ai-era-design.md) and
      [../spec/09-contracts.md](../spec/09-contracts.md) (§2.3, §4).
- [ ] Resolve build-dependent semantics (integer-overflow wrap on release,
      `assert` elision) that conflict with the safety positioning.
