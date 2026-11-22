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
