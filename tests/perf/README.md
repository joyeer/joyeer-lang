# Runtime performance baselines

These are not tests. They measure the runtime so a change to it (decision 0017, thread safety) can
be compared against a baseline on the same machine and configuration.

- `runtime_benchmark` (`cmake --build <dir> --target runtime-benchmark`, a Release build): strings,
  arrays, dictionaries and the raw allocator, in nanoseconds per operation.
- `alloc_heavy.joyeer` and `print_heavy.joyeer`: compile with `joyeer -O2 -o <exe> <file>` and time
  the executable, redirecting `print_heavy`'s output to a file or the null device.
- The size of the compiled `tests/e2e/hello.joyeer` executable at `-O2`.

Record the numbers in the decision or the pull request that changes the runtime, with the machine,
the operating system and the build configuration.

## Comparing two builds

A single timing on a shared machine measures the machine. Build the baseline and the changed
runtime in separate Release trees, then run the two executables **interleaved** (A, B, A, B, ...)
on one pinned core (`taskset -c 3` on Linux), at least 15 times each, and compare the minimum and the
median. Drift then hits both builds equally. The gate for a runtime change is "no regression outside
that noise", plus a stated limit on the growth of the hello-world executable at `-O2`.
