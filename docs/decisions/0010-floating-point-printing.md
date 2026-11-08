# 0010: Floating-point printing in Joyeer

- Status: accepted and implemented (decided and built 2026-11-08). Verified on
  Windows x64 against the UCRT; Linux and macOS have not run it.
- Spec: none. `print` of `Float` and `Double` keeps the output it already had.

## Context

`print` of a `Double` wrote `%.17g` and of a `Float` `%.9g` through `printf`, which a
Joyeer runtime cannot call (variadic) and a libc-free runtime (Tier 2) cannot
depend on. [The runtime plan](../plan/runtime-in-joyeer.md) D6 asked for the same
bytes from Joyeer code, proven against the C library.

## Decision

**Keep the format; replace the implementation.** The output is exactly the C
library's `%.<p>g` with `p` of 9 for a `Float` and 17 for a `Double`: the first `p`
significant digits of the exact binary value, rounded to nearest with an exact tie
to an even last digit, then fixed or exponential notation by the `%g` rule,
trailing zeros dropped, and an exponent of at least two digits (`1e+300`,
`1.5e-07`). Changing to shortest round-trip output would change what programs
print and is a language decision, not this one.

**Exact big-integer digit generation.** A finite value is `mantissa * 2^exponent`.
It is held as `r / s` with both as 1280-bit integers (40 32-bit limbs, enough for
the largest scaled Double, about 1135 bits), scaled by a power of ten so that
`r / s` is in [1, 10). Digits come from repeated subtraction, the remainder is
rounded by comparing `2r` with `s`, and a carry out of the first digit raises the
decimal exponent. This is Dragon4 without the shortest-output termination: simple,
exact for every input and precision, and slower than Ryu. Only the limbs a value
needs are touched, so values near 1 cost a few limbs. Rejected: Ryu or Grisu (large
precomputed tables, or an inexact fast path that still needs this as a fallback),
and 128-bit arithmetic (no such type).

**Scratch memory.** Formatting allocates 576 bytes from the runtime allocator for
the three big integers, the digits and the text, and gives it back. A core unit has
no stack arrays; static slots are shared and not thread-safe. `print` therefore
allocates for a floating-point value (and can panic on out-of-memory), where
integer printing never does.

**One deliberate difference: not-a-number prints `nan`**, with no sign and no
payload text. The C libraries disagree (`-nan` on glibc, `nan` on macOS,
`-nan(ind)` on the Windows UCRT), so printing it identically everywhere is
better than matching one of them. Infinities print `inf` and `-inf`, and zero
prints `0` or `-0`, as before.

**Bit patterns by two intrinsics.** `joyeer_intrinsic_double_bits` and
`joyeer_intrinsic_float_bits` return the IEEE 754 bits of a value (a bit cast).
The language has no `Float`-to-`Double` conversion or bit-level access, and the
decoding needs exact bits.

**Test ABI.** `joyeer_format_double_abi` and `joyeer_format_float_abi` write the
text for a precision of 1 to 17 into a 40-byte caller buffer and return the length,
so tests can compare against `snprintf` directly over many values.

## Verification

The runtime tests compare the Joyeer text with `snprintf("%.*g")` for every power
of two and of ten in range, values that round up across a digit or the fixed/
exponential boundary, exact ties (`2^-25` has 18 digits ending in 5), the extremes
and subnormals, and a precision of 1 to 17; then 100,000 random Double bit
patterns at two precisions, 50,000 decimal-written values and 100,000 random Float
bit patterns. All matched byte for byte on the Windows UCRT.

## Consequences

- No libc formatting remains in the runtime: `runtime.c` has no code left.
- Printing a floating-point value costs a small allocation and a loop over big
  integers instead of one `printf`; it was not benchmarked, and the benchmark gate
  has no floating-point print case.
- The two bit-cast intrinsics are runtime-only like the others (spec §19.4.5).
