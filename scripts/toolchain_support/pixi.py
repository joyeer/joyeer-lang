from __future__ import annotations

import hashlib
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile
from typing import Optional, Sequence, Tuple
import urllib.request

from .process import ToolchainError
from .targets import detect_host


PIXI_VERSION = "0.73.0"
PIXI_ASSETS = {
    "windows-x86_64": (
        "pixi-x86_64-pc-windows-msvc.exe",
        "02c3e1bb4712199f62124deb1b1e9a5ddae32c413d21fda1586e198cd6f9cf2a",
    ),
    "macos-arm64": (
        "pixi-aarch64-apple-darwin",
        "63f335060d0bda2bc67ca487afbe460fc20ffd28e8e8b4878845a206ab972c86",
    ),
    "macos-x86_64": (
        "pixi-x86_64-apple-darwin",
        "280076f71f18e7492064713a779e547ea0c73376c8f14dc3a0b49a80f378d1d2",
    ),
    "linux-arm64": (
        "pixi-aarch64-unknown-linux-musl",
        "539e22c47291fff5e930bfc3654b5a8a5ee809e7ec1a9f75683d8660bdcd4191",
    ),
    "linux-x86_64": (
        "pixi-x86_64-unknown-linux-musl",
        "7127a393da11ff7c76b1fbc458731e24ab8105c3ddb415459cd85fd84a75e715",
    ),
}


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _pixi_version(path: Path) -> Optional[str]:
    try:
        result = subprocess.run(
            [str(path), "--version"],
            check=True,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            timeout=30,
        )
    except (OSError, subprocess.CalledProcessError, subprocess.TimeoutExpired):
        return None
    return result.stdout.strip().removeprefix("pixi ")


def _installed_pixi_candidates(root: Path) -> Tuple[Path, ...]:
    executable = "pixi.exe" if os.name == "nt" else "pixi"
    candidates = [root / ".deps" / "pixi" / PIXI_VERSION / executable]
    discovered = shutil.which("pixi")
    if discovered:
        candidates.append(Path(discovered))
    candidates.append(Path.home() / ".pixi" / "bin" / executable)
    if os.name == "nt" and os.environ.get("LOCALAPPDATA"):
        candidates.append(
            Path(os.environ["LOCALAPPDATA"]) / "pixi" / "bin" / executable
        )
    return tuple(candidates)


def ensure_pixi(root: Path, *, offline: bool) -> Path:
    for candidate in _installed_pixi_candidates(root):
        if candidate.is_file() and _pixi_version(candidate) == PIXI_VERSION:
            return candidate.resolve()

    host = detect_host().key
    try:
        asset, expected_sha256 = PIXI_ASSETS[host]
    except KeyError as error:
        raise ToolchainError(f"Pixi bootstrap does not support '{host}'") from error
    if offline:
        raise ToolchainError(
            f"offline mode requires Pixi {PIXI_VERSION} in PATH or under .deps/pixi; "
            "run bootstrap once online to populate it"
        )

    destination = root / ".deps" / "pixi" / PIXI_VERSION / (
        "pixi.exe" if os.name == "nt" else "pixi"
    )
    url = f"https://github.com/prefix-dev/pixi/releases/download/v{PIXI_VERSION}/{asset}"
    temporary: Optional[Path] = None
    try:
        destination.parent.mkdir(parents=True, exist_ok=True)
        with tempfile.NamedTemporaryFile(
            dir=destination.parent, prefix=".pixi.", delete=False
        ) as output:
            temporary = Path(output.name)
            request = urllib.request.Request(
                url, headers={"User-Agent": "Joyeer-bootstrap/1"}
            )
            with urllib.request.urlopen(request, timeout=120) as response:
                shutil.copyfileobj(response, output)
        if _sha256(temporary) != expected_sha256:
            raise ToolchainError("downloaded Pixi SHA-256 does not match bootstrap data")
        temporary.chmod(temporary.stat().st_mode | stat.S_IXUSR)
        os.replace(temporary, destination)
        temporary = None
        return destination
    except OSError as error:
        raise ToolchainError(f"cannot install Pixi: {error}") from error
    finally:
        if temporary is not None:
            try:
                temporary.unlink(missing_ok=True)
            except OSError:
                pass


def relaunch_in_pixi(
    root: Path, script: Path, arguments: Sequence[str]
) -> Optional[int]:
    if os.environ.get("JOYEER_PIXI_RELAUNCHED") == "1":
        return None
    pixi = ensure_pixi(root, offline="--offline" in arguments)
    command = [
        str(pixi),
        "run",
        "--manifest-path",
        str(root / "pixi.toml"),
        "python",
        str(script),
        *arguments,
    ]
    try:
        environment = os.environ.copy()
        environment["JOYEER_PIXI_RELAUNCHED"] = "1"
        return subprocess.run(
            command, cwd=str(root), env=environment, check=False
        ).returncode
    except OSError as error:
        raise ToolchainError(f"cannot launch Pixi: {error}") from error


def _activate_msvc() -> None:
    program_files_x86 = os.environ.get("ProgramFiles(x86)")
    if not program_files_x86:
        raise ToolchainError("ProgramFiles(x86) is not defined; MSVC cannot be located")
    vswhere = (
        Path(program_files_x86)
        / "Microsoft Visual Studio"
        / "Installer"
        / "vswhere.exe"
    )
    if not vswhere.is_file():
        raise ToolchainError(
            "Visual Studio Build Tools with the MSVC C++ workload are required"
        )
    try:
        query = subprocess.run(
            [
                str(vswhere),
                "-latest",
                "-products",
                "*",
                "-requires",
                "Microsoft.VisualStudio.Component.VC.Tools.x86.x64",
                "-property",
                "installationPath",
            ],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise ToolchainError(f"cannot query Visual Studio: {error}") from error
    if not query.stdout.strip():
        raise ToolchainError(
            "MSVC is incomplete; install Visual Studio's 'Desktop development "
            "with C++' workload and a Windows SDK"
        )
    installation = Path(query.stdout.strip())
    developer_command = installation / "Common7" / "Tools" / "VsDevCmd.bat"
    if not developer_command.is_file():
        raise ToolchainError("Visual Studio's VsDevCmd.bat was not found")
    try:
        result = subprocess.run(
            [
                "cmd.exe",
                "/d",
                "/s",
                "/c",
                "call",
                str(developer_command),
                "-no_logo",
                "-arch=x64",
                "-host_arch=x64",
                ">nul",
                "&&",
                "set",
            ],
            check=True,
            stdout=subprocess.PIPE,
            text=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise ToolchainError(f"cannot activate MSVC: {error}") from error
    for line in result.stdout.splitlines():
        if "=" not in line:
            continue
        name, value = line.split("=", 1)
        if name and not name.startswith("="):
            os.environ[name] = value
    include_paths = [
        Path(value)
        for value in os.environ.get("INCLUDE", "").split(";")
        if value
    ]
    library_paths = [
        Path(value) for value in os.environ.get("LIB", "").split(";") if value
    ]
    has_header = any((path / "string").is_file() for path in include_paths)
    has_runtime = any((path / "libcmt.lib").is_file() for path in library_paths)
    has_sdk = any((path / "kernel32.lib").is_file() for path in library_paths)
    if shutil.which("cl.exe") is None or not (has_header and has_runtime and has_sdk):
        raise ToolchainError(
            "the Visual Studio C++ workload or Windows SDK is incomplete"
        )
    os.environ["CC"] = "cl.exe"
    os.environ["CXX"] = "cl.exe"


def activate_host_compiler() -> None:
    host = detect_host()
    if host.operating_system == "windows":
        _activate_msvc()
    elif host.operating_system == "linux":
        os.environ["CC"] = "gcc"
        os.environ["CXX"] = "g++"
    else:
        os.environ["CC"] = "clang"
        os.environ["CXX"] = "clang++"
