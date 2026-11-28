#!/usr/bin/env python3
"""Run native macOS ARM64 validation for native linking and Fern."""

from __future__ import annotations

import argparse
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
LINK_TESTS = ("JoyeerCInterop.NamedLibrary", "Fern.Plans")
FERN_TESTS = ("Fern.LocalDependencies", "Fern.CompilerDiscovery", "Fern.Installed")
NO_DSYM_TESTS = ("Fern.Bootstrap", "Fern.SelfBuild", "Fern.Installed", "Moss.Serve")
CONFIGS = ("debug", "release")


def capture(*command: str) -> str:
    return subprocess.check_output(command, cwd=ROOT, text=True).strip()


def run(command: list[str], cwd: Path, log: Path, env: dict[str, str], expected: int = 0) -> str:
    print(f"{log.name}: {shlex.join(command)}", flush=True)
    with log.open("x", encoding="utf-8") as output:
        output.write(f"$ {shlex.join(command)}\n")
        output.flush()
        status = subprocess.run(command, cwd=cwd, env=env, stdout=output,
                                stderr=subprocess.STDOUT).returncode
    text = log.read_text(encoding="utf-8", errors="replace")
    if status != expected:
        raise RuntimeError(f"Expected exit {expected}, got {status}: {log}\n" +
                           "\n".join(text.splitlines()[-25:]))
    return text


def require_passes(text: str, tests: tuple[str, ...], log: Path) -> None:
    passed = set(re.findall(
        r"^\s*\d+/\d+\s+Test\s+#\s*\d+:\s+(\S+)\s+\.+\s+Passed\b", text, re.MULTILINE))
    missing = set(tests) - passed
    if missing:
        raise RuntimeError(f"No passing result for {', '.join(sorted(missing))}: {log}")


def focused(checkout: Path, config: str, tests: tuple[str, ...], log: Path, env: dict[str, str]) -> None:
    selector = "^(" + "|".join(re.escape(test) for test in tests) + ")$"
    text = run(["ctest", "--test-dir", str(checkout / "out" / "validate" / config),
                "-R", selector, "--output-on-failure"], checkout, log, env)
    require_passes(text, tests, log)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-root", type=Path, required=True, help="installed LLVM 22.1.8 SDK root")
    parser.add_argument("--lld-dir", type=Path, help="directory with LLDConfig.cmake if installed separately")
    parser.add_argument("--sdk", default="macosx", help="xcrun SDK name, e.g. macosx26.5")
    parser.add_argument("--jobs", type=int, default=3, help="build and unfiltered CTest parallelism")
    args = parser.parse_args()
    if args.jobs < 1:
        parser.error("--jobs must be positive")
    if sys.platform != "darwin" or platform.machine() != "arm64":
        parser.error("run on macOS ARM64 in a native ARM64 terminal, not under Rosetta")
    for tool in ("git", "cmake", "ctest", "ninja", "xcrun", "sw_vers"):
        if shutil.which(tool) is None:
            parser.error(f"{tool} is not available; see docs/building.md")

    try:
        llvm = args.llvm_root.expanduser().resolve()
        llvm_dir = llvm / "lib" / "cmake" / "llvm"
        clang = llvm / "bin" / "clang"
        if not (llvm_dir / "LLVMConfig.cmake").is_file() or not clang.is_file():
            raise RuntimeError(f"{llvm} is not a full LLVM development SDK")
        sdk = capture("xcrun", "--sdk", args.sdk, "--show-sdk-path")
        version = capture("xcrun", "--sdk", args.sdk, "--show-sdk-version")
        if not sdk or not version or not Path(sdk).is_dir():
            raise RuntimeError(f"xcrun did not resolve an installed SDK: {args.sdk}")
        if version.split(".")[:2] in (["27", "0"], ["27", "2"]):
            raise RuntimeError("LLVM/LLD 22.1.8 cannot read this SDK; select an installed 26.5 SDK with --sdk")
        dsymutil = capture("xcrun", "--sdk", args.sdk, "--find", "dsymutil")
        if not Path(dsymutil).is_file() or not os.access(dsymutil, os.X_OK):
            raise RuntimeError("The normal validation build requires an executable dsymutil")
        revision = capture("git", "rev-parse", "HEAD")
        if capture("git", "status", "--porcelain"):
            print("Only committed HEAD will be validated; local edits are not included.", flush=True)

        definitions = {
            "LLVM_DIR": llvm_dir, "JOYEER_CLANG_EXECUTABLE": clang,
            "CMAKE_OSX_ARCHITECTURES": "arm64", "CMAKE_OSX_SYSROOT": sdk,
            "CMAKE_OSX_DEPLOYMENT_TARGET": version, "JOYEER_MACOS_SDK_PATH": sdk,
            "JOYEER_MACOS_SDK_VERSION": version, "JOYEER_MACOS_DEPLOYMENT_TARGET": version,
            "Python3_EXECUTABLE": sys.executable,
        }
        if args.lld_dir is not None:
            lld_dir = args.lld_dir.expanduser().resolve()
            if not (lld_dir / "LLDConfig.cmake").is_file():
                raise RuntimeError(f"{lld_dir} has no LLDConfig.cmake")
            definitions["LLD_DIR"] = lld_dir
        env = dict(os.environ, CMAKE_BUILD_PARALLEL_LEVEL=str(args.jobs), SDKROOT=sdk)
        work = Path(tempfile.mkdtemp(prefix="joyeer-macos-")).resolve()
        print(f"Evidence and isolated checkouts: {work}", flush=True)
        host = capture("sw_vers")
        provenance = (f"{host}\nArchitecture: arm64\nCommit: {revision}\n"
                      f"macOS SDK: {sdk} ({version})\nLLVM root: {llvm}\n"
                      f"LLVM/LLD version required by CMake: 22.1.8\n"
                      f"Normal dsymutil: {dsymutil}\n")
        (work / "provenance.txt").write_text(provenance, encoding="utf-8")

        def checkout(name: str) -> Path:
            path = work / name
            run(["git", "clone", "--no-checkout", "--", str(ROOT), str(path)],
                work, work / f"{name}-clone.log", env)
            run(["git", "checkout", "--detach", revision], path, work / f"{name}-checkout.log", env)
            return path

        def validate(path: Path, stage: str, tool: str, *options: str) -> None:
            command = [sys.executable, "scripts/validate.py", "--llvm-root", str(llvm),
                       "--no-retry", "--jobs", str(args.jobs), "--report", str(work / f"{stage}.md"),
                       "--define", f"JOYEER_DSYMUTIL_EXECUTABLE={tool}", *options]
            for key, value in definitions.items():
                command.extend(("--define", f"{key}={value}"))
            run(command, path, work / f"{stage}.log", env)

        normal = checkout("normal")
        validate(normal, "normal-build", dsymutil, "--skip-ctest", "--skip-fern")
        for config in CONFIGS:
            focused(normal, config, LINK_TESTS, work / f"native-link-{config}.log", env)
        validate(normal, "normal-validation", dsymutil)
        for config in CONFIGS:
            focused(normal, config, FERN_TESTS, work / f"fern-{config}.log", env)

        no_dsym = checkout("no-dsymutil")
        validate(no_dsym, "no-dsymutil-debug", "", "--configs", "Debug")
        build = no_dsym / "out" / "validate" / "debug"
        log = build / "logs" / "ctest.log"
        require_passes(log.read_text(encoding="utf-8"), NO_DSYM_TESTS, log)
        log = work / "explicit-gfull.log"
        output = run([str(build / "bin" / "joyeer"), "-O0", "-gfull", "-o",
                      str(work / "explicit-gfull"), "--", "tests/e2e/hello.joyeer"],
                     no_dsym, log, env, expected=1)
        if not re.search(r"linker[.]missing-tool.*dsymutil", output):
            raise RuntimeError(f"Expected linker.missing-tool for dsymutil: {log}")
        print(f"All macOS handoff gates passed. Return the reports and logs from {work}.")
        return 0
    except (OSError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f"validate-macos: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
