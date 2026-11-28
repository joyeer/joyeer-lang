# Runtime performance baselines

These are not tests. They measure the runtime so a change to it (decision 0017, thread safety) can
be compared against a baseline on the same machine and configuration.

- `runtime_benchmark` (`cmake --build <dir> --target runtime-benchmark`, a Release build): strings,
  arrays, dictionaries and the raw allocator, in nanoseconds per operation.
- `alloc_heavy.joyeer` and `print_heavy.joyeer`: compile with `joyeer -O2 -o <exe> <file>` and time
  the executable, redirecting `print_heavy`'s output to a file or the null device.
- The size of the compiled `tests/e2e/hello.joyeer` executable at `-O2`.
- `task_churn.joyeer` (2048 tasks, batches of 64) and `channel_throughput.joyeer`
  (100000 messages, capacity 64): section 21 migration baselines. Build both profiles with
  `python tests/perf/benchmark_concurrency.py build --debug-compiler <debug-joyeer>
  --release-compiler <release-joyeer> --output-dir <new-directory>`. The directory must not
  already exist, so the baseline executables cannot be overwritten accidentally.

- `array_*.joyeer` and `benchmark_arrays.py` ([decision 0026](../../docs/decisions/0026-library-array.md)): growth by appending, `a[i]` reads and
  writes including a nested array, `removeLast` drains, nested destruction (`[[String]]` build, copy, drop) and
  the linear drain of `[JoinHandle<Int>]`. The script reuses `benchmark_concurrency.py` (same `build` and
  `measure` commands, same interleaved paired comparison and bootstrap) with its own workloads. Each program
  prints one checksum, computed independently in Python, and a run with another output is rejected.

- `array_index_flat`/`array_index_library` and `array_drop_flat`/`array_drop_library` with
  `benchmark_addressor.py` (decision 0026): the builtin array against a minimal library array
  (`array_buffer_prelude.joyeer`), built by the `array-addressor-probe` tool
  (`cmake --build <dir> --target array-addressor-probe`, defined when `JOYEER_BUILD_UNITTESTS` is on), which supplies
  the library through the collections-prelude test seam. To compare the actual product switch instead,
  pass `--compiler <builtin-joyeer> --candidate-compiler <library-joyeer>` instead of `--probe`.
  The same flat program is then compiled by each compiler. `--workload index|drop`, `--debug none|lines|full`. The report treats the builtin
  array as the baseline, so a ratio above 1 is a cost of the library array. The results and the gate numbers are in
  decision 0026, Section 4.

- `dict_*.joyeer` and `benchmark_dicts.py` ([decision 0027](../../docs/decisions/0027-library-dict.md)): Int-key lookup at sizes 1, 4, 16 and
  64 (strict reads, `get` hits and misses), String-key lookup at sizes 4 and 16, insertion with growth to 16 and
  replacement, a four-entry literal with a duplicate key, and nested destruction of `[String: [Int]]`. The same
  `build` and `measure` commands as `benchmark_arrays.py`; each program prints one checksum computed independently in
  Python. Each program runs for 0.2 s or more at `-O2` on a desktop x64 machine, so process startup (about 90 ms on
  Windows) does not dilute the ratios. The `prototype` command (0027, stage 3) builds each program except the literal against its `*_table` twin,
  written over the role-less `Table<K, V>` of `dict_table_prelude.joyeer` with explicit method calls, through
  `array-addressor-probe` with the prelude appended to `lib/Runtime/collections.joyeer`, and measures the pair. Its
  result is a go or no-go verdict only; the gates of 0027 bind at the switch.

- `string_*.joyeer` and `benchmark_strings.py` (decision 0028, stage 3): a `text.count`/`text[i]` byte loop,
  concatenation, equality and ordering, and `utf8()`. The same `build` and `measure` commands as
  `benchmark_arrays.py`; each program prints one checksum computed independently in Python. The baseline is a
  compiler from before stage 3, whose String operations were IR opcodes.

- `equality_*.joyeer` and `benchmark_equality.py` (decision 0031, plan 0012 phase 4): scalar and `String` `==`/`!=`
  (the same program for both compilers), array `==` through `arrayEqual`, and derived struct `==`. A compiler from
  before `Equatable` cannot express the last two, so `build --variant baseline` compiles their hand-written twins
  (`equality_arrays_loop.joyeer`, `equality_derived_fields.joyeer`); `build --variant candidate` compiles the
  originals. `measure` is the shared interleaved paired comparison. Each program prints one checksum computed
  independently in Python.

Record the numbers in the decision or the pull request that changes the runtime, with the machine,
the operating system and the build configuration.

### R0 on macOS ARM64

On 2026-11-28 on a native Apple M3 Pro (`Mac15,6`, macOS 27.0.1 build 26A434),
the frozen `456b78d9` C runtime and its benchmark harness were built with
Apple Clang 21.0.0 at `-O2` against SDK 26.5. The same harness object was
linked against C and against the `513aeed1` Release Joyeer runtime with the
Mach-O and string comparison fixes applied (LLVM/LLD 22.1.8, deployment
target 26.5). The full workload ran for 15 interleaved rounds per comparison,
reversing A/B order each round. Each round reports the best of seven
repetitions; every run finished with a balanced allocator.

| Benchmark | C median/min (ns/op) | Joyeer median/min (ns/op) | Median/min ratio |
|---|---:|---:|---:|
| String equal, equal, 16 B | 1.81/1.73 | 1.82/1.73 | 1.006/1.000 |
| String compare, last byte differs, 16 B | 2.38/2.35 | 2.39/2.25 | 1.004/0.957 |
| String concat + destroy, 16 B | 14.06/13.97 | 11.41/11.18 | 0.812/0.800 |
| String clone + destroy, 16 B | 11.41/11.31 | 8.76/8.61 | 0.768/0.761 |
| String equal, equal, 1024 B | 22.35/22.20 | 22.03/21.68 | 0.986/0.977 |
| String compare, last byte differs, 1024 B | 22.41/22.19 | 22.47/22.35 | 1.003/1.007 |
| String concat + destroy, 1024 B | 35.66/35.59 | 28.78/28.70 | 0.807/0.806 |
| String clone + destroy, 1024 B | 30.61/30.36 | 17.12/17.08 | 0.559/0.563 |
| Array append, grow from empty (`Int`) | 3.34/3.24 | 3.37/3.22 | 1.009/0.994 |
| Array clone + destroy, 1000 elements with callbacks | 1.53/1.53 | 1.50/1.50 | 0.980/0.980 |
| Dictionary set, 1000 distinct `Int` keys | 323.62/322.12 | 145.46/143.17 | 0.449/0.444 |
| Dictionary find, last of 1000 `Int` keys | 622.73/622.56 | 261.69/261.19 | 0.420/0.420 |

Before the string change, 11 rows already met the 10% limit. The 16-byte
unequal compare was 2.47 against C's 2.22 ns/op (1.113 by median and minimum).
In a separate quick sweep, relinking at offsets 0 through 112 in 16-byte
steps within a 128-byte cache line did not change those medians. At every
offset the updated runtime took 2.22 ns/op: it handles nonzero `memcmp`
results separately from the length tie-break, with no size-specific
path. The full run above used the normal Release archive without linker
padding. All rows passed without a performance exception.

### R0 on Linux AArch64

On 2026-11-28, on Oryon AArch64 under Ubuntu 26.04.1/WSL2, the `456b78d9` R0
harness was compiled once with GCC 15.2 `-O2`, then linked against its C
runtime (`runtime.c`, also `-O2`) and the `a362c88` Release Joyeer archive.
Fifteen interleaved rounds pinned to CPU 3 reversed A/B order each round.
Values are ns/op; medians and minima summarize the harness's best of seven
repetitions per round. Every run exited successfully with a balanced allocator.

| Benchmark | C median | Joyeer median | Ratio | C min | Joyeer min |
|---|---:|---:|---:|---:|---:|
| String equal, equal, 16 B | 2.04 | 2.02 | 0.99 | 2.01 | 2.01 |
| String compare, last byte differs, 16 B | 2.51 | 2.26 | 0.90 | 2.51 | 2.25 |
| String concat + destroy, 16 B | 16.15 | 14.87 | 0.92 | 16.12 | 14.81 |
| String clone + destroy, 16 B | 16.19 | 14.63 | 0.90 | 16.12 | 14.42 |
| String equal, equal, 1024 B | 13.49 | 13.21 | 0.98 | 13.36 | 13.10 |
| String compare, last byte differs, 1024 B | 13.99 | 13.72 | 0.98 | 13.86 | 13.61 |
| String concat + destroy, 1024 B | 41.96 | 41.00 | 0.98 | 41.80 | 40.64 |
| String clone + destroy, 1024 B | 27.66 | 39.07 | 1.41 | 26.78 | 31.75 |
| Array append, grow from empty (`Int`) | 3.28 | 7.10 | 2.16 | 3.27 | 7.03 |
| Array clone + destroy, 1000 elements with callbacks | 1.78 | 2.03 | 1.14 | 1.78 | 2.03 |
| Dictionary set, 1000 distinct `Int` keys | 386.90 | 758.60 | 1.96 | 386.80 | 758.50 |
| Dictionary find, last of 1000 `Int` keys | 629.35 | 1506.45 | 2.39 | 629.25 | 1501.65 |

This does not satisfy the 10% gate. AArch64 disassembly shows the candidate
append includes a division for the explicit overflow precheck in `byteCount`,
which the C baseline checks with multiply-high. It is a profiling lead, not
proof that the precheck accounts for all the gaps. The Linux x86-64 and macOS
ARM64 performance gates are separate.

#### Collection hot-path follow-up

A second run used the same `456b78d9` harness object for the C baseline,
unchanged Joyeer runtime, and updated Joyeer runtime. All three ran for
15 interleaved rounds pinned to CPU 3, reversing the run order every round.
The dictionary scan checks the last entry address once and increments by
stride; the integer-key path does not reload the search key on each iteration.
Array append uses Joyeer's checked multiplication instead of a redundant
division and copies eight-byte elements directly. No allocator changes or
performance exceptions were used.

| Benchmark | C median (ns/op) | Before | After | After / C |
|---|---:|---:|---:|---:|
| String clone + destroy, 1024 B | 34.17 | 48.85 | 38.93 | 1.14 |
| Array append, grow from empty (`Int`) | 3.87 | 8.92 | 7.17 | 1.85 |
| Array clone + destroy, 1000 elements with callbacks | 2.09 | 2.38 | 2.38 | 1.14 |
| Dictionary set, 1000 distinct `Int` keys | 454.40 | 891.00 | 174.30 | 0.38 |
| Dictionary find, last of 1000 `Int` keys | 743.20 | 1779.40 | 314.90 | 0.42 |

Both dictionary rows now pass the gate. The other seven R0 rows remained
within the gate, but the three slower rows above still fail it. Their minima
also exceed the corresponding C minima by more than 10%.
An exploratory Linux `mremap` fast path reduced the append cost further in
quick runs, but it still exceeded the limit and did not improve either clone
row. The shipped allocator was left unchanged.

#### Array callback walk

The same frozen harness was run again for 15 interleaved rounds pinned to
CPU 3. The clone and reverse destroy loops now check address bounds once on
their normal path. No append, allocator, or ABI changes from this experiment
were retained.

| Benchmark | C median (ns/op) | Before | After | After / C |
|---|---:|---:|---:|---:|
| String clone + destroy, 1024 B | 33.56 | 40.89 | 38.71 | 1.15 |
| Array append, grow from empty (`Int`) | 3.87 | 7.55 | 7.67 | 1.98 |
| Array clone + destroy, 1000 elements with callbacks | 2.09 | 2.38 | 1.80 | 0.86 |
| Dictionary set, 1000 distinct `Int` keys | 454.30 | 174.80 | 177.10 | 0.39 |
| Dictionary find, last of 1000 `Int` keys | 744.35 | 314.90 | 314.25 | 0.42 |

Array clone/destroy now clears the 10% limit; both remaining misses
also exceed their paired C minima. All seven other rows stayed within
the limit in this run.

#### Allocator growth and class indexing

On 2026-11-28, on Oryon AArch64 under Ubuntu 26.04.1/WSL2
(`6.18.58.1-microsoft-standard-WSL2`), the same `456b78d9` harness object
was linked against frozen C and the before/after Release Joyeer archives.
The harness and C baseline were built with GCC 15.2 `-O2`; the candidate
is `de885a9` plus the allocator changes, built with LLVM 22.1.8 at `-O2`.
Each executable ran the full workload for 15 rounds on CPU 3, reversing
the order each round. The kernel's transparent huge page mode was
`madvise`. Every run exited successfully with a balanced allocator.

| Benchmark | C median/min (ns/op) | Before median/min | After median/min | After / C median/min |
|---|---:|---:|---:|---:|
| String equal, equal, 16 B | 2.40 / 2.36 | 2.39 / 2.37 | 2.40 / 2.36 | 1.000 / 1.000 |
| String compare, last byte differs, 16 B | 2.97 / 2.95 | 2.68 / 2.66 | 2.67 / 2.66 | 0.899 / 0.902 |
| String concat + destroy, 16 B | 20.36 / 19.46 | 18.88 / 18.02 | 18.50 / 17.23 | 0.909 / 0.885 |
| String clone + destroy, 16 B | 20.38 / 19.12 | 18.55 / 17.10 | 18.58 / 17.00 | 0.912 / 0.889 |
| String equal, equal, 1024 B | 16.73 / 16.07 | 16.69 / 15.82 | 16.49 / 15.59 | 0.986 / 0.970 |
| String compare, last byte differs, 1024 B | 17.47 / 16.48 | 17.08 / 16.28 | 17.41 / 16.08 | 0.997 / 0.976 |
| String concat + destroy, 1024 B | 54.04 / 49.97 | 53.46 / 46.40 | 58.04 / 54.10 | 1.074 / 1.083 |
| String clone + destroy, 1024 B | 35.46 / 32.69 | 41.18 / 37.34 | 27.70 / 25.60 | 0.781 / 0.783 |
| Array append, grow from empty (`Int`) | 3.88 / 3.84 | 7.05 / 6.90 | 3.59 / 3.54 | 0.925 / 0.922 |
| Array clone + destroy, 1000 elements with callbacks | 2.09 / 2.09 | 1.80 / 1.80 | 1.79 / 1.79 | 0.856 / 0.856 |
| Dictionary set, 1000 distinct `Int` keys | 454.40 / 454.20 | 181.00 / 165.50 | 182.80 / 166.20 | 0.402 / 0.366 |
| Dictionary find, last of 1000 `Int` keys | 743.50 / 738.85 | 314.25 / 314.05 | 313.85 / 313.55 | 0.422 / 0.424 |

Both previously failing rows pass without an exception. The array code is
unchanged: opportunistic Linux mapping reservation and remapping avoid growth
copies. The allocator computes the same small classes with constant shifts
instead of a variable division; it does not specialize string clone for the
benchmark size. Neither change removes arithmetic overflow checks.

## Comparing two builds

To reproduce the comparison for the merged library collection switch, use
`0d85605` as the candidate and `7a1d748` (`Array`) or `939d210` (`Dict`) as the
builtin baseline. The harnesses measure both `-O0` and `-O2`.

A single timing on a shared machine measures the machine. Build the baseline and the changed
runtime in separate Release trees, then run the two executables **interleaved** (A, B, A, B, ...)
on one pinned core (`taskset -c 3` on Linux), at least 15 times each, and compare the minimum and the
median. Drift then hits both builds equally. The gate for a runtime change is "no regression outside
that noise", plus a stated limit on the growth of the hello-world executable at `-O2`.

For the concurrency fixtures, record the unchanged build with
`python tests/perf/benchmark_concurrency.py measure --baseline-dir <baseline>
--repetitions 15 --report <baseline.json>`. After building the candidate in a different directory,
add `--candidate-dir <candidate>` to measure them interleaved, reversing A/B order on alternate
rounds. The report retains all samples, minima, medians, and executable sizes. Every execution must
produce the expected checksum with no stderr; a timing is not valid evidence if the work failed.
The concurrency gate uses a paired candidate/baseline median ratio and its deterministic
10000-resample 95% bootstrap interval. A lower bound above 1 is a regression outside sampled noise;
do not accept it merely because process startup or other work dominates the total. Keep builds,
validation, and adversarial executions idle while measuring. Retain the samples and increase the
repetition count for an inconclusive/noisy comparison.
