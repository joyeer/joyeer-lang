"""Build and compare the String performance fixtures (decision 0028, stage 3) on one machine.

String operations moved from IR opcodes to the collections prelude's always-inlined String roles. This reuses
the machinery of benchmark_concurrency.py with the String workloads, so the opcode baseline and the prelude
candidate are measured the same way:

    python tests/perf/benchmark_strings.py build --debug-compiler <debug-joyeer> \
        --release-compiler <release-joyeer> --output-dir <new-directory>
    python tests/perf/benchmark_strings.py measure --baseline-dir <baseline> \
        [--candidate-dir <candidate>] --repetitions 15 --report <report.json>
"""

import benchmark_concurrency as shared

# Each program prints one checksum, computed independently of the compiler (see the README).
shared.WORKLOADS.clear()
shared.WORKLOADS.update(
    {
        "string_bytes": "21401600000\n",
        "string_concat": "18000000\n",
        "string_compare": "75000000\n",
        "string_utf8": "62000000\n",
    }
)

if __name__ == "__main__":
    shared.main()
