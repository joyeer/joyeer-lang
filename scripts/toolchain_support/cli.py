from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Optional, Sequence

from .process import (
    ExecutionContext,
    ToolchainError,
    find_program,
    require_program,
    run,
)
from .targets import Host, detect_host


MINIMUM_PYTHON = (3, 9)


def _require_python() -> None:
    if sys.version_info < MINIMUM_PYTHON:
        required = ".".join(str(value) for value in MINIMUM_PYTHON)
        actual = f"{sys.version_info.major}.{sys.version_info.minor}"
        raise ToolchainError(f"Python {required}+ is required; found {actual}")


def _repository_root() -> Path:
    return Path(__file__).resolve().parents[2]


def _add_execution_options(parser: argparse.ArgumentParser) -> None:
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Print actions without changing files, downloading, or launching tools.",
    )
    parser.add_argument(
        "--verbose",
        action="store_true",
        help="Print commands and selected paths.",
    )


def _context(arguments: argparse.Namespace) -> ExecutionContext:
    return ExecutionContext(dry_run=arguments.dry_run, verbose=arguments.verbose)


def _status(host: Host) -> None:
    print(f"Host: {host.key}")
    print(f"Python: {sys.version.split()[0]} ({sys.executable})")
    for program in ("cmake", "ninja", "clang", "lld"):
        print(f"{program}: {find_program(program) or 'not found'}")


def _run_tests(
    root: Path,
    build_directory: Path,
    context: ExecutionContext,
) -> None:
    run(
        [
            sys.executable,
            "-m",
            "unittest",
            "discover",
            "-s",
            "scripts/tests",
            "-v",
        ],
        context=context,
        cwd=root,
    )
    _build(root, build_directory, context)
    ctest = require_program("ctest")
    run(
        [ctest, "--test-dir", str(build_directory), "--output-on-failure"],
        context=context,
        cwd=root,
    )


def _build(
    root: Path,
    build_directory: Path,
    context: ExecutionContext,
) -> None:
    cmake = require_program("cmake")
    run([cmake, "--build", str(build_directory)], context=context, cwd=root)


def _toolchain_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build and validate Joyeer's pinned native toolchain."
    )
    _add_execution_options(parser)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="Show the active development tools.")

    build = commands.add_parser("build", help="Build the configured Joyeer tree.")
    build.add_argument("--build-dir", type=Path, default=Path("build"))

    test = commands.add_parser(
        "test", help="Run automation tests, build Joyeer, and run unfiltered CTest."
    )
    test.add_argument("--build-dir", type=Path, default=Path("build"))
    return parser


def toolchain_main(arguments: Optional[Sequence[str]] = None) -> int:
    try:
        _require_python()
        parsed = _toolchain_parser().parse_args(arguments)
        root = _repository_root()
        host = detect_host()
        context = _context(parsed)
        if parsed.command == "status":
            _status(host)
        elif parsed.command == "build":
            _build(root, parsed.build_dir, context)
        elif parsed.command == "test":
            _run_tests(root, parsed.build_dir, context)
        return 0
    except ToolchainError as error:
        print(f"toolchain error: {error}", file=sys.stderr)
        return 1


def _bootstrap_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Prepare Joyeer's locked development environment and configure CMake."
    )
    _add_execution_options(parser)
    parser.add_argument("--offline", action="store_true", help="Forbid network access.")
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument(
        "--no-tests", action="store_true", help="Configure without C++ unit tests."
    )
    return parser


def bootstrap_main(arguments: Optional[Sequence[str]] = None) -> int:
    try:
        _require_python()
        parsed = _bootstrap_parser().parse_args(arguments)
        root = _repository_root()
        context = _context(parsed)
        cmake = require_program("cmake")
        ninja = require_program("ninja")
        clang = Path(require_program("clang"))
        build_directory = parsed.build_dir
        if not build_directory.is_absolute():
            build_directory = root / build_directory
        command = [
            cmake,
            "--fresh",
            "-S",
            str(root),
            "-B",
            str(build_directory),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DJOYEER_BUILD_UNITTESTS={'OFF' if parsed.no_tests else 'ON'}",
            f"-DJOYEER_CLANG_EXECUTABLE={clang}",
        ]
        if parsed.offline:
            command.append("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        run(command, context=context, cwd=root)
        if not context.dry_run:
            print(f"Joyeer is configured in {build_directory}")
        return 0
    except ToolchainError as error:
        print(f"bootstrap error: {error}", file=sys.stderr)
        return 1