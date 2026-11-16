"""Build and compare the dictionary performance fixtures (plan 0011, phase 4, decision 0027) on one machine.

This reuses the machinery of benchmark_concurrency.py with a different set of workloads, so the baseline of the
builtin dictionary and the candidate of the library dictionary are measured the same way:

    python tests/perf/benchmark_dicts.py build --debug-compiler <debug-joyeer> \
        --release-compiler <release-joyeer> --output-dir <new-directory>
    python tests/perf/benchmark_dicts.py measure --baseline-dir <baseline> \
        [--candidate-dir <candidate>] --repetitions 25 --report <report.json>

The stage 3 prototype (0027) compares the builtin dictionary with the `Table<K, V>` of dict_table_prelude.joyeer:

    python tests/perf/benchmark_dicts.py prototype --debug-compiler <debug-joyeer> \
        --release-compiler <release-joyeer> --debug-probe <debug-array-addressor-probe> \
        --release-probe <release-array-addressor-probe> --output-dir <new-directory> --report <report.json>

It builds each builtin program with the compiler and its `_table` twin with the probe of the same build tree (so
both link the same runtime), the `-O0` profile from the Debug tree and the `-O2` profile from the Release tree as
`build` does, then measures them interleaved. The literal is not part of the prototype (0027: it arrives with the
switch).
"""

import argparse
from pathlib import Path
import sys

import benchmark_concurrency as shared

# Each program prints one checksum, computed independently of the compiler (see the README).
shared.WORKLOADS.clear()
shared.WORKLOADS.update(
    {
        "dict_lookup": "126332000\n",
        "dict_lookup_string": "72720000\n",
        "dict_insert_growth": "16100000\n",
        "dict_literal": "12500022500000\n",
        "dict_nested_destroy": "5001850000\n",
    }
)

PROTOTYPE_WORKLOADS = ("dict_lookup", "dict_lookup_string", "dict_insert_growth", "dict_nested_destroy")
HERE = Path(__file__).resolve().parent
REPOSITORY = HERE.parent.parent


def prototype(args):
    workloads = {name: shared.WORKLOADS[name] for name in PROTOTYPE_WORKLOADS}
    shared.WORKLOADS.clear()
    shared.WORKLOADS.update(workloads)
    baseline = args.output_dir / "builtin"
    candidate = args.output_dir / "table"
    baseline.mkdir(parents=True, exist_ok=False)
    candidate.mkdir(parents=True, exist_ok=False)
    # The replaced collections prelude: the product's library array followed by the prototype dictionary.
    prelude = args.output_dir / "table_prelude.joyeer"
    with open(prelude, "w", encoding="utf-8", newline="\n") as output:
        for part in (REPOSITORY / "lib" / "Runtime" / "collections.joyeer", HERE / "dict_table_prelude.joyeer"):
            output.write(part.read_text(encoding="utf-8"))
    for profile, compiler, probe in zip(
        shared.PROFILES, (args.debug_compiler, args.release_compiler), (args.debug_probe, args.release_probe)
    ):
        for workload, expected in workloads.items():
            builtin = shared.executable(baseline, workload, profile)
            shared.run_checked(
                [str(compiler), "-" + profile, "-g0", "-o", str(builtin), str(HERE / (workload + ".joyeer"))]
            )
            table = shared.executable(candidate, workload, profile)
            shared.run_checked(
                [str(probe), str(HERE / (workload + "_table.joyeer")), str(prelude), str(table), "--" + profile]
            )
            for path in (builtin, table):
                shared.sample(path, expected)
    shared.measure(
        argparse.Namespace(
            baseline_dir=baseline, candidate_dir=candidate, repetitions=args.repetitions, report=args.report
        )
    )


def main():
    if len(sys.argv) < 2 or sys.argv[1] != "prototype":
        shared.main()
        return
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("command", choices=["prototype"])
    parser.add_argument("--debug-compiler", type=Path, required=True)
    parser.add_argument("--release-compiler", type=Path, required=True)
    parser.add_argument("--debug-probe", type=Path, required=True)
    parser.add_argument("--release-probe", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=25)
    args = parser.parse_args()
    if args.repetitions < 1:
        parser.error("--repetitions must be at least 1")
    prototype(args)


if __name__ == "__main__":
    main()
