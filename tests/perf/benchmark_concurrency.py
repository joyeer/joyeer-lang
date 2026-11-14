"""Build and compare the section 21 performance fixtures on one machine."""

import argparse
import json
import os
from pathlib import Path
import random
import statistics
import subprocess
import time


WORKLOADS = {
    "task_churn": "2098176\n",
    "channel_throughput": "5000050000\n100000\n",
}
PROFILES = ("O0", "O2")
SUFFIX = ".exe" if os.name == "nt" else ""


def executable(directory, workload, profile):
    return directory / (workload + "-" + profile + SUFFIX)


def run_checked(command):
    completed = subprocess.run(command, capture_output=True, text=True, timeout=120)
    if completed.returncode != 0:
        raise RuntimeError(
            "{} failed ({})\n{}{}".format(
                command, completed.returncode, completed.stdout, completed.stderr
            )
        )
    return completed


def sample(path, expected):
    start = time.perf_counter()
    completed = run_checked([str(path)])
    elapsed = time.perf_counter() - start
    if completed.stdout != expected or completed.stderr:
        raise RuntimeError(
            "{} produced unexpected output: stdout={!r}, stderr={!r}".format(
                path, completed.stdout, completed.stderr
            )
        )
    return elapsed


def build(args):
    args.output_dir.mkdir(parents=True, exist_ok=False)
    for profile, compiler in zip(
        PROFILES, (args.debug_compiler, args.release_compiler)
    ):
        for workload in WORKLOADS:
            path = executable(args.output_dir, workload, profile)
            run_checked(
                [
                    str(compiler),
                    "-" + profile,
                    "-g0",
                    "-o",
                    str(path),
                    str(Path(__file__).resolve().parent / (workload + ".joyeer")),
                ]
            )
            sample(path, WORKLOADS[workload])


def measure(args):
    report = {"repetitions": args.repetitions, "workloads": {}}
    directories = {"baseline": args.baseline_dir}
    if args.candidate_dir is not None:
        directories["candidate"] = args.candidate_dir
    for profile in PROFILES:
        for workload, expected in WORKLOADS.items():
            paths = {
                label: executable(directory, workload, profile)
                for label, directory in directories.items()
            }
            for path in paths.values():
                sample(path, expected)
            samples = {label: [] for label in paths}
            for repetition in range(args.repetitions):
                labels = list(paths)
                if repetition % 2:
                    labels.reverse()
                for label in labels:
                    samples[label].append(sample(paths[label], expected))
            result = {
                label: {
                    "seconds": values,
                    "minimum": min(values),
                    "median": statistics.median(values),
                    "executable_bytes": paths[label].stat().st_size,
                }
                for label, values in samples.items()
            }
            if "candidate" in result:
                result["median_change_percent"] = 100 * (
                    result["candidate"]["median"] / result["baseline"]["median"] - 1
                )
                ratios = [
                    candidate / baseline
                    for baseline, candidate in zip(samples["baseline"], samples["candidate"])
                ]
                rng = random.Random(0)
                medians = sorted(
                    statistics.median(rng.choices(ratios, k=len(ratios)))
                    for _ in range(10000)
                )
                interval = [medians[249], medians[9749]]
                result["paired_median_ratio"] = statistics.median(ratios)
                result["paired_median_ratio_ci95"] = interval
                result["regression_outside_noise"] = interval[0] > 1
            report["workloads"][workload + "-" + profile] = result
    text = json.dumps(report, indent=2) + "\n"
    args.report.write_text(text, encoding="utf-8")
    print(text, end="")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    builder = commands.add_parser("build")
    builder.add_argument("--debug-compiler", type=Path, required=True)
    builder.add_argument("--release-compiler", type=Path, required=True)
    builder.add_argument("--output-dir", type=Path, required=True)
    builder.set_defaults(action=build)
    measurer = commands.add_parser("measure")
    measurer.add_argument("--baseline-dir", type=Path, required=True)
    measurer.add_argument("--candidate-dir", type=Path)
    measurer.add_argument("--repetitions", type=int, default=15)
    measurer.add_argument("--report", type=Path, required=True)
    measurer.set_defaults(action=measure)
    args = parser.parse_args()
    if args.command == "measure" and args.repetitions < 1:
        parser.error("--repetitions must be at least 1")
    args.action(args)


if __name__ == "__main__":
    main()
