#!/usr/bin/env python3
"""Stage an existing Joyeer Release build with audited dependency notices."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys


class InstallError(Exception):
    pass


def build_options(build: Path, source: Path) -> dict[str, str]:
    cache_path = build / "CMakeCache.txt"
    if not cache_path.is_file() or not (build / "cmake_install.cmake").is_file():
        raise InstallError(f"Configure and build Joyeer Release before staging: {build}")
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        if line.startswith(("#", "//")):
            continue
        key_and_type, separator, value = line.partition("=")
        key, colon, _ = key_and_type.partition(":")
        if separator and colon:
            cache[key] = value
    home = cache.get("CMAKE_HOME_DIRECTORY")
    if cache.get("CMAKE_PROJECT_NAME") != "joyeer" or not home or Path(home).resolve() != source:
        raise InstallError("The Release build must belong to this Joyeer checkout.")
    generator = cache.get("CMAKE_GENERATOR", "")
    if generator in {"Ninja Multi-Config", "Xcode"} or generator.startswith("Visual Studio "):
        release = "Release" in cache.get("CMAKE_CONFIGURATION_TYPES", "").split(";")
    else:
        release = cache.get("CMAKE_BUILD_TYPE") == "Release"
    if not release:
        raise InstallError("Use a Release configuration.")
    if any(enabled(cache, flag) for flag in (
            "JOYEER_BACKEND_TEST_HOOKS", "JOYEER_SANITIZE_HOST", "JOYEER_SANITIZE_RUNTIME")):
        raise InstallError("Release staging requires backend test hooks and sanitizers to be off.")
    return cache


def enabled(cache: dict[str, str], key: str) -> bool:
    value = cache.get(key, "").upper()
    if value in {"1", "ON", "YES", "TRUE", "Y"}:
        return True
    if value in {"", "0", "OFF", "NO", "FALSE", "N", "IGNORE", "NOTFOUND"} or value.endswith("-NOTFOUND"):
        return False
    raise InstallError(f"Use a canonical ON/OFF CMake BOOL for {key}, not {cache[key]!r}.")


def notice_files(directory: Path, system: str) -> list[Path]:
    required = {"LLVM.txt", "LLD.txt", "Platform.txt"}
    if system == "Windows":
        required.add("LibXml2.txt")
    elif system == "Linux":
        required.add("Clang.txt")
    paths = sorted(directory.iterdir()) if directory.is_dir() else []
    present = {path.name for path in paths}
    if missing := sorted(required - present):
        raise InstallError(f"Missing dependency notices in {directory}: {', '.join(missing)}")
    names = set()
    for path in paths:
        name = path.name.casefold()
        if path.suffix.lower() != ".txt" or name in names:
            raise InstallError(f"Invalid or duplicate notice filename: {path.name}")
        if path.is_symlink() or not path.is_file() or not path.stat().st_size:
            raise InstallError(f"Notice must be a nonempty regular file: {path}")
        names.add(name)
    return paths


def inventory(directory: Path) -> dict[str, dict[str, int | str]]:
    files = {}
    for path in sorted(directory.rglob("*")):
        if path.is_symlink() or (not path.is_file() and not path.is_dir()):
            raise InstallError(f"Nonregular release entry: {path}")
        if not path.is_file():
            continue
        digest = hashlib.sha256()
        size = 0
        with path.open("rb") as stream:
            for block in iter(lambda: stream.read(1024 * 1024), b""):
                digest.update(block)
                size += len(block)
        if size == 0:
            raise InstallError(f"Empty release file: {path}")
        files[path.relative_to(directory).as_posix()] = {"sha256": digest.hexdigest(), "size": size}
    return files


def stage_release(build: Path, destination: Path, notices: Path, source: Path, system: str) -> None:
    if system not in {"Windows", "Darwin", "Linux"}:
        raise InstallError(f"Unsupported operating system: {system}")
    cache = build_options(build, source)
    destination = destination.parent.resolve(strict=True) / destination.name
    if destination == build or build in destination.parents:
        raise InstallError("Stage outside the build tree.")
    notice_paths = notice_files(notices, system)
    if destination.exists() or destination.is_symlink():
        raise InstallError(f"Release destination already exists: {destination}")
    cmake = shutil.which("cmake")
    if cmake is None:
        raise InstallError("CMake is not on PATH.")
    components = ["JoyeerRuntime", "StandardLibrary"]
    suffix = ".exe" if system == "Windows" else ""
    backend = {
        "Windows": "joyeer-backend.dll",
        "Darwin": "libjoyeer-backend.dylib",
        "Linux": "libjoyeer-backend.so",
    }[system]
    archive = "JoyeerRuntime.lib" if system == "Windows" else "libJoyeerRuntime.a"
    required = {f"joyeer{suffix}", backend, f"lib/{archive}", "licenses/LICENSE",
                "lib/index.toml", "lib/Foundation/project.toml", "lib/System/project.toml"}
    for module in ("Foundation", "System"):
        source_files = list((source / "lib" / module).glob("*.joyeer"))
        if not source_files:
            raise InstallError(f"No {module} sources in the Joyeer checkout.")
        required.update(path.relative_to(source).as_posix() for path in source_files)
    compression = enabled(cache, "JOYEER_BUILD_COMPRESSION")
    moss = enabled(cache, "JOYEER_BUILD_MOSS")
    if compression:
        compression_archive = "JoyeerCompression.lib" if system == "Windows" else "libJoyeerCompression.a"
        required.update({"lib/Compression/project.toml", "lib/Compression/compression.joyeer",
                         "lib/Compression/licenses/Joyeer-LICENSE", "lib/Compression/licenses/miniz-LICENSE",
                         f"lib/Compression/native/{compression_archive}"})
    if enabled(cache, "JOYEER_BUILD_FERN"):
        components.append("PRODUCT")
        required.add(f"fern{suffix}")
        if compression and moss:
            components.append("Moss")
            required.add(f"moss{suffix}")

    try:
        destination.mkdir()
    except FileExistsError as error:
        raise InstallError(f"Release destination already exists: {destination}") from error
    environment = dict(os.environ)
    environment.pop("DESTDIR", None)
    for component in components:
        try:
            subprocess.run([cmake, "--install", str(build), "--config", "Release",
                            "--component", component, "--prefix", str(destination)],
                           check=True, env=environment)
        except subprocess.CalledProcessError as error:
            raise InstallError(f"{component} installation failed ({error.returncode}). "
                               f"Incomplete stage retained at {destination}") from error
    for path in notice_paths:
        target = destination / "licenses" / path.name
        with path.open("rb") as original, target.open("xb") as copy:
            shutil.copyfileobj(original, copy)
    files = inventory(destination)
    if missing := sorted(required - files.keys()):
        raise InstallError(f"Missing release products: {', '.join(missing)}")
    report = {"schema": 1, "configuration": "Release", "platform": system, "files": files}
    with (destination / "release-inventory.json").open("x", encoding="utf-8", newline="\n") as output:
        json.dump(report, output, indent=2, sort_keys=True)
        output.write("\n")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
    parser.add_argument("--build-dir", required=True, type=Path, help="existing native Release build")
    parser.add_argument("--notice-dir", required=True, type=Path, help="audited SDK and dependency notice files")
    parser.add_argument("--output-dir", required=True, type=Path, help="new directory for product staging")
    args = parser.parse_args()
    try:
        destination = args.output_dir.absolute()
        stage_release(args.build_dir.resolve(), destination, args.notice_dir.resolve(),
                      Path(__file__).resolve().parents[1], platform.system())
    except (InstallError, OSError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"Staged Release at {destination}. Verify dependency and relocation gates before distribution.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
