from __future__ import annotations

import argparse
from pathlib import Path
import sys
from typing import Optional, Sequence

from .config import LlvmLock, LockError, load_llvm_lock
from .llvm import archive_is_valid, fetch_llvm, source_is_current
from .paths import RepositoryPaths
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


def _load() -> tuple[RepositoryPaths, LlvmLock, Host]:
    paths = RepositoryPaths.discover()
    lock = load_llvm_lock(paths.llvm_lock)
    host = detect_host()
    lock.targets_for(host.key)
    return paths, lock, host


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


def _status(paths: RepositoryPaths, lock: LlvmLock, host: Host) -> None:
    archive = paths.downloads / lock.source.filename
    source = paths.sources / lock.version
    print(f"Host: {host.key}")
    print(f"Python: {sys.version.split()[0]} ({sys.executable})")
    print(f"LLVM: {lock.version} ({lock.commit})")
    print(f"LLVM lock: {paths.llvm_lock}")
    print(f"LLVM targets: {';'.join(lock.targets_for(host.key))}")
    print(f"Archive: {'verified' if archive_is_valid(archive, lock.source) else 'missing'}")
    print(f"Source: {'ready' if source_is_current(source, lock) else 'missing'}")
    for program in ("cmake", "ninja"):
        print(f"{program}: {find_program(program) or 'not found'}")


def _run_tests(
    paths: RepositoryPaths,
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
        cwd=paths.root,
    )
    cmake = require_program("cmake")
    run([cmake, "--build", str(build_directory)], context=context, cwd=paths.root)
    ctest = require_program("ctest")
    run(
        [ctest, "--test-dir", str(build_directory), "--output-on-failure"],
        context=context,
        cwd=paths.root,
    )


def _not_implemented(command: str) -> None:
    raise ToolchainError(
        f"'{command}' is reserved by the toolchain plan but is not implemented yet"
    )


def _toolchain_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Build and validate Joyeer's pinned native toolchain."
    )
    _add_execution_options(parser)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="Show host, tools, and pinned LLVM state.")

    fetch = commands.add_parser(
        "fetch-llvm", help="Download, verify, and safely extract pinned LLVM source."
    )
    fetch.add_argument("--offline", action="store_true", help="Forbid network access.")
    fetch.add_argument(
        "--force", action="store_true", help="Replace cached archive and source state."
    )
    fetch.add_argument(
        "--archive-only", action="store_true", help="Verify the archive without extraction."
    )

    test = commands.add_parser(
        "test", help="Run automation tests, build Joyeer, and run unfiltered CTest."
    )
    test.add_argument("--build-dir", type=Path, default=Path("build"))

    for name in ("build-llvm", "build-codegen", "package", "verify-package"):
        commands.add_parser(name, help=f"Reserved: {name.replace('-', ' ')}.")
    return parser


def toolchain_main(arguments: Optional[Sequence[str]] = None) -> int:
    try:
        _require_python()
        parsed = _toolchain_parser().parse_args(arguments)
        paths, lock, host = _load()
        context = _context(parsed)
        if parsed.command == "status":
            _status(paths, lock, host)
        elif parsed.command == "fetch-llvm":
            result = fetch_llvm(
                lock,
                paths,
                offline=parsed.offline,
                force=parsed.force,
                archive_only=parsed.archive_only,
                context=context,
            )
            print(result)
        elif parsed.command == "test":
            _run_tests(paths, parsed.build_dir, context)
        else:
            _not_implemented(parsed.command)
        return 0
    except (LockError, ToolchainError) as error:
        print(f"toolchain error: {error}", file=sys.stderr)
        return 1


def _bootstrap_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Initialize a Joyeer source checkout for CMake + Ninja development."
    )
    _add_execution_options(parser)
    parser.add_argument("--offline", action="store_true", help="Forbid network access.")
    parser.add_argument(
        "--force", action="store_true", help="Replace cached LLVM source when fetching."
    )
    parser.add_argument(
        "--fetch-llvm-source",
        action="store_true",
        help="Download and extract the pinned LLVM source before configuring Joyeer.",
    )
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument(
        "--clang",
        type=Path,
        help="Clang driver for the current transitional native path.",
    )
    parser.add_argument(
        "--no-tests", action="store_true", help="Configure without C++ unit tests."
    )
    return parser


def bootstrap_main(arguments: Optional[Sequence[str]] = None) -> int:
    try:
        _require_python()
        parsed = _bootstrap_parser().parse_args(arguments)
        paths, lock, host = _load()
        context = _context(parsed)
        cmake = require_program("cmake")
        ninja = require_program("ninja")
        context.log(f"Host: {host.key}")
        context.log(f"LLVM lock: {lock.version} ({lock.commit})")
        if parsed.fetch_llvm_source:
            fetch_llvm(
                lock,
                paths,
                offline=parsed.offline,
                force=parsed.force,
                archive_only=False,
                context=context,
            )

        build_directory = parsed.build_dir
        if not build_directory.is_absolute():
            build_directory = paths.root / build_directory
        command = [
            cmake,
            "-S",
            str(paths.root),
            "-B",
            str(build_directory),
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={ninja}",
            f"-DJOYEER_BUILD_UNITTESTS={'OFF' if parsed.no_tests else 'ON'}",
        ]
        if parsed.offline:
            command.append("-DFETCHCONTENT_FULLY_DISCONNECTED=ON")
        if parsed.clang is not None:
            clang = parsed.clang.expanduser().resolve()
            if not clang.is_file():
                raise ToolchainError(f"Clang executable is not a file: '{clang}'")
            command.append(f"-DJOYEER_CLANG_EXECUTABLE={clang}")
        run(command, context=context, cwd=paths.root)
        if not context.dry_run:
            print(f"Joyeer is configured in {build_directory}")
        return 0
    except (LockError, ToolchainError) as error:
        print(f"bootstrap error: {error}", file=sys.stderr)
        return 1