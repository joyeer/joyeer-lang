"""Build and compare the equality performance fixtures (decision 0029, plan 0012 phase 4) on one machine.

Scalar and String equality compile the same program with both compilers. A compiler from before `Equatable` has no
array `==` and no derived conformances, so for those two workloads it compiles a hand-written twin
(`*_loop.joyeer`, `*_fields.joyeer`) that does the same comparisons. Measuring reuses benchmark_concurrency.py:

    python tests/perf/benchmark_equality.py build --variant baseline --compiler <old-joyeer> \
        --output-dir <new-directory>
    python tests/perf/benchmark_equality.py build --variant candidate --compiler <new-joyeer> \
        --output-dir <new-directory>
    python tests/perf/benchmark_equality.py measure --baseline-dir <baseline> \
        --candidate-dir <candidate> --repetitions 30 --report <report.json>
"""

import argparse
from pathlib import Path
import sys

import benchmark_concurrency as shared

# Each program prints one checksum, computed independently of the compiler (see the README).
shared.WORKLOADS.clear()
shared.WORKLOADS.update(
    {
        "equality_scalars": "298000000\n",
        "equality_strings": "30000000\n",
        "equality_arrays": "2500000\n",
        "equality_derived": "87500000\n",
    }
)

# The source a baseline compiler builds for a workload it cannot express.
BASELINE_SOURCES = {
    "equality_arrays": "equality_arrays_loop",
    "equality_derived": "equality_derived_fields",
}


def build(args):
    args.output_dir.mkdir(parents=True, exist_ok=False)
    here = Path(__file__).resolve().parent
    for profile in shared.PROFILES:
        for workload, expected in shared.WORKLOADS.items():
            source = workload
            if args.variant == "baseline":
                source = BASELINE_SOURCES.get(workload, workload)
            path = shared.executable(args.output_dir, workload, profile)
            shared.run_checked(
                [
                    str(args.compiler),
                    "-" + profile,
                    "-g0",
                    "-o",
                    str(path),
                    str(here / (source + ".joyeer")),
                ]
            )
            shared.sample(path, expected)


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "build":
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument("command")
        parser.add_argument("--variant", choices=("baseline", "candidate"), required=True)
        parser.add_argument("--compiler", type=Path, required=True)
        parser.add_argument("--output-dir", type=Path, required=True)
        build(parser.parse_args())
        return
    shared.main()


if __name__ == "__main__":
    main()
