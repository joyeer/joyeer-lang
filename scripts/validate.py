#!/usr/bin/env python3
"""Configure, build, and test Joyeer, then print one platform x config x suite table.

This replaces the hand-run "configure, build, ctest, joypm test" handoff on each
platform. It never installs or resolves a toolchain: CMake, Ninja, a C++20
compiler (a Developer shell on Windows), and the LLVM/LLD/Clang 22.1.8 SDK
(--llvm-root, JOYEER_LLVM_ROOT, or LLVM_HOME) must already exist. See
docs/building.md.

    python3 scripts/validate.py --llvm-root /opt/llvm
    python3 scripts/validate.py --configs Release --report validation.md

Exit status is 0 only when every selected suite passed.
"""

from __future__ import annotations

import argparse
import os
import platform
import re
import shutil
import subprocess
import sys
import time
from dataclasses import dataclass
from pathlib import Path
from typing import Optional

ROOT = Path(__file__).resolve().parent.parent
EXE = ".exe" if os.name == "nt" else ""
BUILD_ROOT = ROOT / "out" / "validate"


class ValidationError(Exception):
    pass


@dataclass
class Row:
    config: str
    suite: str
    ok: bool
    summary: str
    seconds: float


def run(command: list[str], log: Path, env: dict[str, str], cwd: Path = ROOT) -> tuple[int, str]:
    """Run an argument array (never a shell string); keep the full output in a log file."""
    log.parent.mkdir(parents=True, exist_ok=True)
    completed = subprocess.run(
        command,
        cwd=cwd,
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        errors="replace",
    )
    log.write_text(f"$ {' '.join(command)}\n{completed.stdout}", encoding="utf-8")
    return completed.returncode, completed.stdout


def tail(text: str, lines: int = 25) -> str:
    return "\n".join(text.strip().splitlines()[-lines:])


def llvm_root(argument: Optional[str]) -> Path:
    value = argument or os.environ.get("JOYEER_LLVM_ROOT") or os.environ.get("LLVM_HOME")
    if not value:
        raise ValidationError("set --llvm-root, JOYEER_LLVM_ROOT, or LLVM_HOME to the LLVM/LLD/Clang 22.1.8 SDK")
    path = Path(value).expanduser().resolve()
    if not (path / "lib" / "cmake" / "llvm").is_dir():
        raise ValidationError(f"{path} has no lib/cmake/llvm; point at the full development SDK")
    return path


def child_environment(root: Path, notes: list[str]) -> dict[str, str]:
    env = dict(os.environ)
    if os.name == "nt" and shutil.which("cl", path=env.get("PATH")) is None:
        raise ValidationError("cl.exe is not on PATH; run from a Developer shell for the target architecture")
    if sys.platform.startswith("linux") and shutil.which("ld.lld", path=env.get("PATH")) is None:
        # The in-process Clang Driver only checks that an ld.lld file exists for -fuse-ld=lld.
        if (root / "bin" / "ld.lld").exists():
            env["PATH"] = str(root / "bin") + os.pathsep + env.get("PATH", "")
            notes.append(f"ld.lld was not on PATH; added {root / 'bin'} for the child processes only")
        else:
            raise ValidationError("ld.lld is neither on PATH nor in the SDK bin directory; the Linux link step needs it")
    return env


def safe_remove(directory: Path) -> None:
    resolved = directory.resolve()
    if BUILD_ROOT.resolve() not in resolved.parents:
        raise ValidationError(f"refusing to delete {resolved}: not under {BUILD_ROOT}")
    shutil.rmtree(resolved, ignore_errors=True)


def parse_ctest(output: str) -> tuple[bool, str, list[str]]:
    # Some CMake versions omit ", N tests failed" when everything passed.
    found = re.search(r"(\d+)% tests passed(?:, (\d+) tests? failed)? out of (\d+)", output)
    if not found:
        return False, "no ctest summary", []
    failed, total = int(found.group(2) or 0), int(found.group(3))
    names: list[str] = []
    capture = False
    for line in output.splitlines():
        if "The following tests FAILED" in line:
            capture = True
        elif capture:
            match = re.match(r"\s*\d+ - (\S+) \((.+?)\)", line)
            if match:
                names.append(f"{match.group(1)} ({match.group(2)})")
            elif line.strip() == "":
                capture = False
    return failed == 0 and total > 0, f"{total - failed} / {total} passed", names


def parse_joypm(output: str) -> tuple[bool, str]:
    found = re.search(r"(\d+) tests?: (\d+) passed, (\d+) build-fail, (\d+) run-fail", output)
    if not found:
        return False, "no joypm summary"
    total, passed, build_fail, run_fail = (int(found.group(i)) for i in range(1, 5))
    detail = f"{passed} / {total} passed"
    if build_fail or run_fail:
        detail += f" ({build_fail} build-fail, {run_fail} run-fail)"
    return build_fail == 0 and run_fail == 0 and passed == total and total > 0, detail


def validate_config(config: str, args: argparse.Namespace, sdk: Path, env: dict[str, str]) -> list[Row]:
    rows: list[Row] = []
    build_dir = BUILD_ROOT / config.lower()
    logs = build_dir / "logs"
    if args.fresh and build_dir.exists():
        safe_remove(build_dir)

    def record(suite: str, ok: bool, summary: str, started: float) -> None:
        rows.append(Row(config, suite, ok, summary, time.monotonic() - started))
        print(f"  [{'PASS' if ok else 'FAIL'}] {config} {suite}: {summary}", flush=True)

    started = time.monotonic()
    configure = [
        "cmake", "-S", str(ROOT), "-B", str(build_dir), "-G", "Ninja",
        f"-DCMAKE_BUILD_TYPE={config}",
        f"-DJOYEER_LLVM_ROOT={sdk}",
        "-DJOYEER_BUILD_UNITTESTS=ON",
        f"-DJOYEER_BUILD_COMPRESSION={'OFF' if args.no_compression else 'ON'}",
        *[f"-D{item}" for item in args.define],
    ]
    code, output = run(configure, logs / "configure.log", env)
    if code != 0:
        record("configure", False, "failed\n" + tail(output), started)
        return rows
    started = time.monotonic()
    code, output = run(["cmake", "--build", str(build_dir)], logs / "build.log", env)
    if code != 0:
        record("build", False, "failed\n" + tail(output), started)
        return rows
    record("build", True, "ok", started)

    if args.tests_list:
        code, listing = run(["ctest", "--test-dir", str(build_dir), "-N"], logs / "ctest-list.log", env)
        names = sorted(m.group(1).strip() for m in re.finditer(r"Test\s+#\d+: (.+)", listing))
        target = Path(args.tests_list)
        target.mkdir(parents=True, exist_ok=True)
        label = f"{platform.system().lower()}-{platform.machine().lower()}-{config.lower()}"
        (target / f"ctest-{label}.txt").write_text("\n".join(names) + "\n", encoding="utf-8")

    if not args.skip_ctest:
        started = time.monotonic()
        command = ["ctest", "--test-dir", str(build_dir), "-j", str(args.jobs), "--output-on-failure"]
        code, output = run(command, logs / "ctest.log", env)
        ok, summary, failed = parse_ctest(output)
        if failed and not args.no_retry:
            # Rerun only the failures once. A test that passes on the rerun is reported, not hidden.
            again, rerun = run(["ctest", "--test-dir", str(build_dir), "--rerun-failed", "--output-on-failure"],
                               logs / "ctest-rerun.log", env)
            rerun_ok, _, still_failed = parse_ctest(rerun)
            if rerun_ok and again == 0:
                total = re.search(r"out of (\d+)", output)
                count = total.group(1) if total else "?"
                summary = f"{count} / {count} passed; {len(failed)} passed only on rerun: " + ", ".join(name.split(" (")[0] for name in failed[:8])
                failed, ok, code = [], True, 0
            else:
                failed = still_failed or failed
        if failed:
            summary += "; failed: " + ", ".join(failed[:12]) + (" ..." if len(failed) > 12 else "")
        record("ctest", ok and code == 0, summary, started)

    if not args.skip_joypm:
        joypm = build_dir / "bin" / f"joypm{EXE}"
        compiler = build_dir / "bin" / f"joyeer{EXE}"
        started = time.monotonic()
        if not joypm.exists():
            record("joypm test", False, f"{joypm} was not built (JOYEER_BUILD_JOYPM=OFF?)", started)
        else:
            command = [str(joypm), "test", "--compiler", str(compiler), "--workspace", str(ROOT / "workspace.toml")]
            if config.lower() == "release":
                command.append("--release")
            code, output = run(command, logs / "joypm.log", env)
            ok, summary = parse_joypm(output)
            record("joypm test", ok and code == 0, summary, started)
    return rows


def git_state() -> str:
    def git(*words: str) -> str:
        done = subprocess.run(["git", *words], cwd=ROOT, capture_output=True, text=True)
        return done.stdout.strip() if done.returncode == 0 else ""

    commit = git("rev-parse", "--short", "HEAD") or "unknown"
    dirty = " (uncommitted changes)" if git("status", "--porcelain") else ""
    return f"{git('rev-parse', '--abbrev-ref', 'HEAD') or '?'} @ {commit}{dirty}"


def table(rows: list[Row]) -> str:
    where = f"{platform.system()} {platform.machine()}"
    lines = ["| Platform | Config | Suite | Result | Seconds |", "|---|---|---|---|---|"]
    for row in rows:
        first = row.summary.splitlines()[0] if row.summary else ""
        lines.append(f"| {where} | {row.config} | {row.suite} | {'pass' if row.ok else 'FAIL'}: {first} | {row.seconds:.0f} |")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--llvm-root", help="LLVM/LLD/Clang 22.1.8 SDK (default: JOYEER_LLVM_ROOT, then LLVM_HOME)")
    parser.add_argument("--configs", default="Debug,Release", help="comma-separated: Debug, Release (default both)")
    parser.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="ctest parallelism")
    parser.add_argument("--no-compression", action="store_true", help="skip the optional miniz-backed Compression library")
    parser.add_argument("--define", action="append", default=[], metavar="VAR=VALUE",
                        help="extra CMake cache entry, for example FETCHCONTENT_SOURCE_DIR_MINIZ=/path (repeatable)")
    parser.add_argument("--no-retry", action="store_true", help="do not rerun failed ctest tests once")
    parser.add_argument("--skip-ctest", action="store_true")
    parser.add_argument("--skip-joypm", action="store_true")
    parser.add_argument("--fresh", action="store_true", help=f"delete the build tree under {BUILD_ROOT} first")
    parser.add_argument("--tests-list", metavar="DIR", help="write the sorted ctest test names to DIR, to diff platforms")
    parser.add_argument("--report", metavar="FILE", help="also write the table (Markdown) to FILE")
    args = parser.parse_args()

    configs = [item.strip().capitalize() for item in args.configs.split(",") if item.strip()]
    if not configs or any(item not in ("Debug", "Release") for item in configs):
        parser.error("--configs takes Debug and/or Release")
    try:
        sdk = llvm_root(args.llvm_root)
        notes: list[str] = []
        env = child_environment(sdk, notes)
    except ValidationError as error:
        print(f"validate: {error}", file=sys.stderr)
        return 2

    print(f"validate: {git_state()}\nvalidate: LLVM SDK {sdk}\nvalidate: logs under {BUILD_ROOT}")
    for note in notes:
        print(f"validate: note: {note}")
    rows: list[Row] = []
    try:
        for config in configs:
            print(f"== {config} ==", flush=True)
            rows.extend(validate_config(config, args, sdk, env))
    except ValidationError as error:
        print(f"validate: {error}", file=sys.stderr)
        return 2

    report = table(rows)
    print("\n" + report)
    failures = [row for row in rows if not row.ok]
    for row in failures:
        if "\n" in row.summary:
            print(f"\n{row.config} {row.suite}:\n{row.summary}")
    if args.report:
        Path(args.report).write_text(f"{git_state()}\n\n{report}\n", encoding="utf-8")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
