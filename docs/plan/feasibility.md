# Joyeer — Design Feasibility

> Can the Joyeer design actually be built? This doc separates two questions
> that are easy to conflate:
>
> 1. **Is the design theoretically/engineering-wise possible?** — mostly *yes*.
> 2. **Is the v0.1 implementation there yet?** — the scoped native JSON-parser
>    acceptance target is complete; the broader future language is not (see
>    [v0.1.md](v0.1.md) and [roadmap.md](roadmap.md)).
>
> This document answers **(1)**. It records the current conclusion so we can
> revisit and expand it later. Where it touches *what the language is*, the
> normative source remains [../spec.md](../spec.md); the *why* remains
> [../rationale/](../rationale/).

---

## TL;DR

- **The systems-language core has strong engineering precedent.** Related
  languages demonstrate many of its components, and Joyeer's native MVP
  demonstrates a scoped integration. This is evidence of feasibility, not
  proof that every proposed feature combination or performance target works.
- **The main near-term risks are integration, ergonomics, scope, and
  implementation quality.** Ownership soundness, ABI costs, platform behavior,
  and diagnostics need broader workloads and regression coverage.
- **The one genuinely hard part — fully automatic formal verification of
  arbitrary properties — has a theoretical ceiling and should *not* be
  promised.** A layered approach (runtime checks, a precisely defined decidable
  subset, then optional SMT assistance) can still be useful without making
  unsupported comparisons to other languages.

---

## 1. Related features have prior art

Joyeer is not betting on an unsolved research problem; it is betting on
**engineering integration**. The table below maps each major design commitment
to related work. The mechanisms and maturity of those projects differ;
similar terminology does not establish equivalent semantics.

| Feature | Prior art (proof it works) | Notes |
|---|---|---|
| Value semantics and ownership | **Rust, Swift, Hylo** | Useful related designs, not identical memory models. Swift also uses ARC, whereas Joyeer excludes it; Hylo is a value-oriented language project, not evidence that Joyeer's full design has shipped. |
| Parameter access conventions | **Swift** `borrowing`, `consuming`, `inout` | [SE-0377](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0377-parameter-ownership-modifiers.md), implemented in Swift 5.9, documents related syntax, not Joyeer's `initializing` contract. Joyeer's four-convention model needs its own specification and implementation. |
| Subscript projections and `yield` | **Swift** `_read` / `_modify` coroutine accessors | Related implementation techniques, not proof of Joyeer's four-accessor semantics. |
| Runtime checks and verification | **Eiffel, Ada/SPARK, Dafny, Verus** | These illustrate different checking/proof strategies. Joyeer specifies ordinary `assert` / `precondition` / `fatalError` calls, not `requires` / `ensures` declarations (§9). |
| Refinement types (`PositiveInt`, `NonEmpty`) | **Liquid Haskell, F\***, Dafny | Decidable subsets are well understood. |
| Zero-cost generics (monomorphization) | **Rust, C++** | Standard practice. |
| LLVM backend | **Rust, Swift, Clang** | Large but fully known path. |

**Conclusion:** there is substantial prior art, but combining these mechanisms
still requires checking their assumptions, performance, and user-facing rules.

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
"borrow checking is annoying" to NLL to Polonius. Joyeer now has the minimal
checker: it enforces call-site and argument-evaluation exclusivity for the
v0.1 non-escaping projection surface, with direct diagnostics tests and the
native JSON parser as an integration workload. Longer-lived `yield`
projections and broader ergonomic stress cases remain future work.

### 2.2 Index-alias safety — *solvable, known answer*

Banning reference types (spec §4.8) pushes graph/cyclic structures toward
integer indices. Exclusivity and bounds checks do not establish an index's
container identity or generation: a stale or cross-container index can name
the wrong live element even while remaining in bounds. This is a logical
identity problem, distinct from dereferencing a freed native pointer.

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

**Key insight:** Joyeer does not need Layer 4 to be valuable. Layers 2 and 3
would need explicitly bounded proof obligations and an honest treatment of
unproved properties. None of these proposed verification layers is evidence
of current implementation support.

---

## 3. Verdict

- ✅ **The scoped native systems-language core is implemented.** This supports
  the architectural direction, but broader soundness, runtime-cost, and
  release-quality claims still require evidence. Refinement types and
  verification are separate future design work.

- ⚠️ **As "AI generates, verifier proves arbitrary correctness"** — has a
  theoretical ceiling and will **never** be 100% automatic. It also doesn't
  need to be; landing at SMT-assisted (Layer 3) is implementable and already
  strong.

In short: prioritize proving the current subset's invariants and costs before
expanding its surface. Prior art supports feasibility; it does not remove
design risk or establish the project's resource and schedule requirements.

---

## 4. Recommended spec note (to de-risk the flagship claim)

Keep current guarantees distinct from proposed verification work:

> The current compiler checks the implemented type, control-flow, and ownership
> rules. Runtime contract APIs, refinements, and SMT assistance require
> additional design and implementation. Automatic proof of arbitrary program
> correctness is not a release promise.

The layer model above is an evaluation framework, not a commitment to deliver
Layer 3 in a particular release.

---

## Open items (to revisit later)

- [x] Prototype the minimal v0.1 exclusivity checker and validate call-site and
  argument-evaluation conflicts (§2.1).
- [ ] Stress-test exclusivity ergonomics on broader mutation-heavy workloads
  before adding longer-lived projections (§2.1).
- [ ] Design the checked `Handle<T>` / arena abstraction for index-alias
      safety, or document the unsafe boundary explicitly (§2.2).
- [ ] Decide the layered-verification commitment and reflect it in
      [../rationale/ai-era-design.md](../rationale/ai-era-design.md) and
      [../spec/09-contracts.md](../spec/09-contracts.md) (§2.3, §4).
- [x] Make integer overflow trap in every build mode.
- [ ] When implementing the check APIs, preserve the distinction already
  specified in §9: optimized builds may elide `assert`, but not required
  `precondition`, arithmetic, or bounds-check semantics.
