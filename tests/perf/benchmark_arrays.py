"""Build and compare the array performance fixtures (plan 0011, phase 3) on one machine.

This reuses the machinery of benchmark_concurrency.py with a different set of workloads, so the
baseline of the builtin array and the candidate of the library array are measured the same way:

    python tests/perf/benchmark_arrays.py build --debug-compiler <debug-joyeer> \
        --release-compiler <release-joyeer> --output-dir <new-directory>
    python tests/perf/benchmark_arrays.py measure --baseline-dir <baseline> \
        [--candidate-dir <candidate>] --repetitions 15 --report <report.json>
"""

import benchmark_concurrency as shared

# Each program prints one checksum, computed independently of the compiler (see the README).
shared.WORKLOADS.clear()
shared.WORKLOADS.update(
    {
        "array_append_growth": "8224000\n",
        "array_index": "227831808000\n",
        "array_remove_last": "392448000\n",
        "array_nested_destroy": "72000\n",
        "array_linear_drain": "295296\n",
    }
)

if __name__ == "__main__":
    shared.main()
