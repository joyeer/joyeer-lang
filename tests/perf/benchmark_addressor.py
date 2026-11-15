"""Measure the cost of a library array's element access against the builtin array (decision 0026, stage 2).

Builds array_index_flat.joyeer with the compiler (the builtin `a[i]`) and array_index_library.joyeer with
array-addressor-probe (the `array_element` addressor of 0025, over the minimal library in
array_buffer_prelude.joyeer), at -O0 and -O2, then times the two interleaved and compares them with the paired
bootstrap of benchmark_concurrency.py. The report treats the builtin array as the baseline and the library
array as the candidate, so a ratio above 1 is a cost of the library array.

    python tests/perf/benchmark_addressor.py --compiler <joyeer> --probe <array-addressor-probe> \
        --output-dir <new-directory> --report <report.json> [--debug none|lines|full] [--repetitions 15]
"""

import argparse
from pathlib import Path
import subprocess

import benchmark_concurrency as shared

# name -> (builtin program, library program, expected output). Both programs of a pair print the same value.
PAIRS = {
    "index": ("array_index_flat", "array_index_library", "221165568000\n"),
    "drop": ("array_drop_flat", "array_drop_library", "8000000\n"),
}
shared.WORKLOADS.clear()

HERE = Path(__file__).resolve().parent
DEBUG_FLAGS = {"none": ["-g0"], "lines": ["-g"], "full": ["-gfull"]}
PROBE_FLAGS = {"none": [], "lines": ["--lines"], "full": ["--full"]}


def build(args):
    flat_name, library_name, expected = PAIRS[args.workload]
    shared.WORKLOADS[flat_name] = expected
    baseline = args.output_dir / "builtin"
    candidate = args.output_dir / "library"
    baseline.mkdir(parents=True, exist_ok=False)
    candidate.mkdir(parents=True, exist_ok=False)
    for profile in shared.PROFILES:
        flat = shared.executable(baseline, flat_name, profile)
        shared.run_checked(
            [str(args.compiler), "-" + profile, *DEBUG_FLAGS[args.debug], "-o", str(flat),
             str(HERE / (flat_name + ".joyeer"))]
        )
        library = shared.executable(candidate, flat_name, profile)
        shared.run_checked(
            [str(args.probe), str(HERE / (library_name + ".joyeer")),
             str(HERE / "array_buffer_prelude.joyeer"), str(library), "--" + profile,
             *PROBE_FLAGS[args.debug]]
        )
        for path in (flat, library):
            shared.sample(path, expected)
    return baseline, candidate


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--probe", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--workload", choices=sorted(PAIRS), default="index")
    parser.add_argument("--debug", choices=sorted(DEBUG_FLAGS), default="none")
    parser.add_argument("--repetitions", type=int, default=15)
    args = parser.parse_args()
    baseline, candidate = build(args)
    shared.measure(
        argparse.Namespace(
            baseline_dir=baseline, candidate_dir=candidate,
            repetitions=args.repetitions, report=args.report,
        )
    )


if __name__ == "__main__":
    main()
