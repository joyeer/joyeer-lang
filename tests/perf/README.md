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

- `array_*.joyeer` and `benchmark_arrays.py` (plan 0011, phase 3): growth by appending, `a[i]` reads and
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

Record the numbers in the decision or the pull request that changes the runtime, with the machine,
the operating system and the build configuration.

## Comparing two builds

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
