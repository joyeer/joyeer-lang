## §10 Effects 🔬 (syntax-only in v0.1)

### 10.1 Pure by default

Any function without a `performs` clause is **pure**: it may only read
its `borrowing` parameters, write through its `inout`/`initializing`
parameters, allocate via `consuming` returns (covered by the `Alloc` effect,
see below),
and call other pure functions.

This is the AI-era guarantee: functions without `performs` annotations
have no observable side effects beyond data flow.

### 10.2 `performs` clause

```
effect_clause   ::= 'performs' effect_label ( ',' effect_label )*
effect_label    ::= identifier [ '<' type , ... '>' ]
```

```joyeer
func readFile(path: String): String
  performs IO, Throw<FileError>
{
  // ...
}
```

### 10.3 Built-in effect labels (v0.1 vocabulary)

| Label | Meaning |
|-------|---------|
| `IO` | Filesystem, network, terminal access. |
| `Throw<E>` | May produce a `.Err(E)` even from a `Result`-typed return. (Annotation for analysis; semantics in §8.) |
| `Async` | Suspends and resumes. Reserved ⏳; used once concurrency lands. |
| `Alloc` | Performs heap allocation. Pure functions may still allocate transient buffers used to produce their return value. |
| `Random` | Non-deterministic. |
| `Time`  | Reads wall-clock or monotonic time. |

### 10.4 Effect polymorphism

Deferred. v0.1 effects are concrete sets only — no `performs <E>` generic.

### 10.5 v0.1 compiler treatment

- Parsed and stored on the function symbol.
- Propagated: if `f` calls `g`, `f`'s effects must be a superset of `g`'s.
  Violation is a **warning** in v0.1, a **hard error** in v0.2.
- Not used for codegen.

---

