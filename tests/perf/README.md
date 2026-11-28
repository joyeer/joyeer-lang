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
