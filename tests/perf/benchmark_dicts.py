"""Build and compare the dictionary performance fixtures (plan 0011, phase 4, decision 0027) on one machine.

This reuses the machinery of benchmark_concurrency.py with a different set of workloads, so the baseline of the
builtin dictionary and the candidate of the library dictionary are measured the same way:

    python tests/perf/benchmark_dicts.py build --debug-compiler <debug-joyeer> \
        --release-compiler <release-joyeer> --output-dir <new-directory>
    python tests/perf/benchmark_dicts.py measure --baseline-dir <baseline> \
        [--candidate-dir <candidate>] --repetitions 25 --report <report.json>
"""

import benchmark_concurrency as shared

# Each program prints one checksum, computed independently of the compiler (see the README).
shared.WORKLOADS.clear()
shared.WORKLOADS.update(
    {
        "dict_lookup": "12633200\n",
        "dict_lookup_string": "1454400\n",
        "dict_insert_growth": "161000\n",
        "dict_literal": "1250225000\n",
        "dict_nested_destroy": "134250\n",
    }
)

if __name__ == "__main__":
    shared.main()
