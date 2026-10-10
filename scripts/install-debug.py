#!/usr/bin/env python3

from __future__ import annotations

import argparse
import ctypes
import ntpath
import os
import platform
import re
import shlex
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Optional


class InstallError(Exception):
    pass


@dataclass
class DebugBuild:
    directory: Path
    artifacts: dict[str, Path]
    symbols: dict[str, Path]
    install_joypm: bool


def native_windows_architecture() -> str:
    import winreg

    try:
        with winreg.OpenKey(
            winreg.HKEY_LOCAL_MACHINE,
            r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
            0,
            winreg.KEY_READ | winreg.KEY_WOW64_64KEY,
        ) as key:
            architecture, _ = winreg.QueryValueEx(key, "PROCESSOR_ARCHITECTURE")
    except OSError as error:
        raise InstallError(
            "Cannot identify the Windows system architecture; specify --build-dir."
        ) from error
    return str(architecture).upper()


def default_debug_build_dir(source_dir: Path, system: str) -> Path:
    if system == "Windows":
        architecture = native_windows_architecture()
        presets = {"ARM64": "arm64-debug", "AMD64": "x64-debug"}
        if architecture not in presets:
            raise InstallError(
                f"Unsupported Windows architecture '{architecture}'; specify --build-dir."
            )
        preset = presets[architecture]
    elif system == "Darwin":
        preset = "macos-debug"
    elif system == "Linux":
        preset = "linux-debug"
    else:
        raise InstallError(f"Unsupported operating system: {system}.")
    return source_dir / "out" / "build" / preset


def read_cache(build_dir: Path) -> dict[str, str]:
    cache_path = build_dir / "CMakeCache.txt"
    if not cache_path.is_file():
        raise InstallError(f"No CMakeCache.txt in {build_dir}; configure and build Debug first.")
    cache = {}
    for line in cache_path.read_text(encoding="utf-8").splitlines():
        match = re.match(r"^([^/#][^:]*):[^=]+=(.*)$", line)
        if match:
            cache[match[1]] = match[2]
    return cache


def require_file(path: Path, description: str) -> None:
    if not path.is_file() or path.stat().st_size == 0:
        raise InstallError(f"Missing or empty {description}: {path}.")


def require_dsym(path: Path) -> None:
    if not path.is_dir():
        raise InstallError(f"Missing Debug dSYM bundle: {path}. Build Debug first.")
    require_file(path / "Contents" / "Info.plist", "Debug dSYM metadata")
    require_file(
        path / "Contents" / "Resources" / "DWARF" / path.name.removesuffix(".dSYM"),
        "Debug DWARF artifact",
    )


def inspect_debug_build(build_dir: Path, source_dir: Path, system: str) -> DebugBuild:
    if not build_dir.is_dir():
        raise InstallError(
            f"Debug build directory not found: {build_dir}. "
            "Build the matching Debug preset first, or specify --build-dir."
        )
    cache = read_cache(build_dir)
    home = cache.get("CMAKE_HOME_DIRECTORY")
    if cache.get("CMAKE_PROJECT_NAME") != "joyeer" or not home or Path(home).resolve() != source_dir:
        raise InstallError("The build must belong to this Joyeer checkout.")

    output_dir = build_dir
    configurations = cache.get("CMAKE_CONFIGURATION_TYPES")
    if configurations:
        if "Debug" not in configurations.split(";"):
            raise InstallError("The selected build does not support Debug.")
        output_dir /= "Debug"
    elif cache.get("CMAKE_BUILD_TYPE") != "Debug":
        raise InstallError("Only Debug builds can be installed with this script.")

    binary_dir = output_dir / "bin"
    library_dir = output_dir / "lib"
    install_joypm = cache.get("JOYEER_BUILD_JOYPM", "").upper() in {"1", "ON", "YES", "TRUE", "Y"}
    symbols = {}
    if system == "Windows":
        artifacts = {
            "joyeer.exe": binary_dir / "joyeer.exe",
            "joyeer-backend.dll": binary_dir / "joyeer-backend.dll",
            "JoyeerNativeRuntime.lib": library_dir / "JoyeerNativeRuntime.lib",
        }
        symbols = {name: binary_dir / name for name in ("joyeer.pdb", "joyeer-backend.pdb")}
        if install_joypm:
            artifacts["joypm.exe"] = binary_dir / "joypm.exe"
            symbols["joypm.pdb"] = binary_dir / "joypm.pdb"
    elif system in {"Darwin", "Linux"}:
        backend_name = "libjoyeer-backend.dylib" if system == "Darwin" else "libjoyeer-backend.so"
        artifacts = {
            "joyeer": binary_dir / "joyeer",
            backend_name: library_dir / backend_name,
            "libJoyeerNativeRuntime.a": library_dir / "libJoyeerNativeRuntime.a",
        }
        if install_joypm:
            artifacts["joypm"] = binary_dir / "joypm"
        if system == "Darwin":
            for name, artifact in artifacts.items():
                if name == "libJoyeerNativeRuntime.a":
                    continue
                bundle = artifact.with_name(name + ".dSYM")
                dsymutil = cache.get("JOYEER_DSYMUTIL_EXECUTABLE", "")
                required = name == "joypm" and bool(dsymutil) and Path(dsymutil).is_file()
                if required or bundle.exists():
                    require_dsym(bundle)
                    symbols[bundle.name] = bundle
    else:
        raise InstallError(f"Unsupported operating system: {system}.")

    for artifact in artifacts.values():
        require_file(artifact, "Debug artifact")
    for symbol in symbols.values():
        if not symbol.is_dir():
            require_file(symbol, "Debug artifact")
    if not (build_dir / "cmake_install.cmake").is_file():
        raise InstallError("Missing cmake_install.cmake; reconfigure the build first.")
    return DebugBuild(build_dir, artifacts, symbols, install_joypm)


def require_directory_path(path: Path, description: str) -> None:
    for parent in (path, *path.parents):
        if (parent.exists() or parent.is_symlink()) and not parent.is_dir():
            raise InstallError(f"{description} is not a directory: {parent}.")


def validate_install_directory(install_dir: Path, source_dir: Path, build: DebugBuild) -> None:
    if (
        install_dir == source_dir
        or install_dir.is_relative_to(build.directory)
        or install_dir == Path(install_dir.anchor)
    ):
        raise InstallError(
            "Choose an install directory outside the build tree and not the source or filesystem root."
        )
    require_directory_path(install_dir, "Install destination")
    for name, symbol in build.symbols.items():
        destination = install_dir / name
        if symbol.is_dir():
            require_directory_path(destination, "Debug symbol destination")
        elif destination.exists() and not destination.is_file():
            raise InstallError(f"Debug symbol destination is not a file: {destination}.")


def prepare_skill_files(
    source: Path, destination: Path, force: bool
) -> list[tuple[Path, Path]]:
    if not (source / "SKILL.md").is_file():
        raise InstallError(f"Missing Joyeer skill: {source}.")
    require_directory_path(destination, "Skill destination")
    empty_destination = not destination.exists() or not any(destination.iterdir())
    pending = []
    for source_file in sorted(source.rglob("*")):
        if not source_file.is_file():
            continue
        relative = source_file.relative_to(source)
        target = destination / relative
        if target.is_dir():
            raise InstallError(f"Skill file destination is a directory: {target}.")
        require_directory_path(target.parent, "Skill parent path")
        if target.is_file() and source_file.read_bytes() == target.read_bytes():
            continue
        if not empty_destination and not force:
            raise InstallError(
                f"Existing Joyeer skill differs at '{relative}' in {destination}. "
                "Review and back up your changes before using --force, "
                "choose --skill-dir, or use --skip-skill-install."
            )
        if target.exists() or target.is_symlink():
            if not target.is_file():
                raise InstallError(f"Skill file destination is not a regular file: {target}.")
        pending.append((source_file, target))
    return pending


def append_windows_path(path_value: str, entry: str, additional_path: str = "") -> str:
    normalized_entry = ntpath.normcase(ntpath.normpath(ntpath.expandvars(entry)))
    for candidate in (path_value + ";" + additional_path).split(";"):
        candidate = candidate.strip().strip('"')
        if candidate and ntpath.normcase(ntpath.normpath(ntpath.expandvars(candidate))) == normalized_entry:
            return path_value
    if not path_value.strip():
        return entry
    return path_value.rstrip(";") + ";" + entry


def read_windows_path(hive: int, key_name: str) -> tuple[str, int]:
    import winreg

    try:
        with winreg.OpenKey(hive, key_name) as key:
            value, value_type = winreg.QueryValueEx(key, "Path")
    except FileNotFoundError:
        return "", winreg.REG_EXPAND_SZ
    if not isinstance(value, str) or value_type not in {winreg.REG_SZ, winreg.REG_EXPAND_SZ}:
        raise InstallError(f"Windows PATH is not a string value in {key_name}.")
    return value, value_type


def broadcast_environment_change() -> None:
    from ctypes import wintypes

    send = ctypes.WinDLL("user32", use_last_error=True).SendMessageTimeoutW
    send.argtypes = [
        wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPCWSTR,
        wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t),
    ]
    send.restype = wintypes.LPARAM
    result = ctypes.c_size_t()
    if not send(0xFFFF, 0x001A, 0, "Environment", 0x0002, 1000, ctypes.byref(result)):
        raise InstallError(
            "User PATH was updated, but Windows environment notification failed "
            f"(error {ctypes.get_last_error()}); restart your terminal host or sign in again."
        )


def update_windows_path(install_dir: Path) -> None:
    import winreg

    user_path, value_type = read_windows_path(winreg.HKEY_CURRENT_USER, "Environment")
    machine_path, _ = read_windows_path(
        winreg.HKEY_LOCAL_MACHINE,
        r"SYSTEM\CurrentControlSet\Control\Session Manager\Environment",
    )
    updated = append_windows_path(user_path, str(install_dir), machine_path)
    if updated != user_path:
        with winreg.CreateKeyEx(winreg.HKEY_CURRENT_USER, "Environment", 0, winreg.KEY_SET_VALUE) as key:
            winreg.SetValueEx(key, "Path", 0, value_type, updated)
        broadcast_environment_change()
    print("Install directory is on user/system PATH. Restart your terminal host or IDE to pick it up.")


def validate_path_entry(install_dir: Path, system: str) -> None:
    separator = ";" if system == "Windows" else ":"
    if any(character in str(install_dir) for character in (separator, "\r", "\n", "\0")):
        raise InstallError(
            "The install directory cannot be represented as a PATH entry; "
            "choose another directory or use --skip-path-update."
        )


def prepare_posix_path_update(
    install_dir: Path, home: Path, system: str
) -> tuple[Path, Optional[bytes]]:
    shell_path = os.environ.get("SHELL")
    if not shell_path:
        import pwd

        shell_path = pwd.getpwuid(os.getuid()).pw_shell
    shell = Path(shell_path).name
    entry = str(install_dir)
    quoted = shlex.quote(entry)
    if shell == "zsh":
        profile = Path(os.environ.get("ZDOTDIR") or home).expanduser() / ".zshrc"
    elif shell == "bash":
        profile = home / ".bashrc"
        if system == "Darwin":
            profiles = [home / name for name in (".bash_profile", ".bash_login", ".profile")]
            profile = next((candidate for candidate in profiles if candidate.exists()), profiles[0])
    elif shell in {"sh", "dash", "ksh"}:
        profile = home / ".profile"
    elif shell == "fish":
        config_dir = Path(os.environ.get("XDG_CONFIG_HOME") or home / ".config").expanduser()
        profile = config_dir / "fish" / "conf.d" / "joyeer.fish"
    else:
        raise InstallError(
            f"Unsupported shell '{shell}' for automatic PATH setup; "
            "use --skip-path-update and add the install directory to PATH manually."
        )
    profile = profile.resolve()
    require_directory_path(profile.parent, "Shell profile parent")
    if (profile.exists() or profile.is_symlink()) and not profile.is_file():
        raise InstallError(f"Shell profile is not a regular file: {profile}.")
    if shell == "fish":
        quoted = "'" + entry.replace("\\", "\\\\").replace("'", "\\'") + "'"
        block = (
            "# Joyeer Debug installer\n"
            f"if not contains -- {quoted} $PATH\n"
            f"    set -gx PATH $PATH {quoted}\n"
            "end\n"
        )
    else:
        block = (
            "# Joyeer Debug installer\n"
            'case ":${PATH-}:" in\n'
            f"    *:{quoted}:*) ;;\n"
            f'    *) export PATH="${{PATH:+$PATH:}}"{quoted} ;;\n'
            "esac\n"
        )
    content = profile.read_bytes() if profile.exists() else b""
    encoded = block.encode("utf-8")
    if encoded in content:
        return profile, None
    newline = b"\n" if content and not content.endswith(b"\n") else b""
    return profile, content + newline + encoded


def install_debug(args: argparse.Namespace, source_dir: Path, system: str) -> None:
    if system not in {"Windows", "Darwin", "Linux"}:
        raise InstallError(f"Unsupported operating system: {system}.")
    build_dir = args.build_dir
    if build_dir is None:
        build_dir = default_debug_build_dir(source_dir, system)
        print(f"Selected native Debug build: {build_dir}")
    build = inspect_debug_build(build_dir.expanduser().resolve(), source_dir, system)
    install_dir = args.install_dir.expanduser().resolve()
    validate_install_directory(install_dir, source_dir, build)
    skill_dir = args.skill_dir.expanduser().resolve()
    skill_files = []
    if not args.skip_skill_install:
        skill_files = prepare_skill_files(source_dir / "skills" / "joyeer", skill_dir, args.force)
    profile_update = None
    if not args.skip_path_update:
        validate_path_entry(install_dir, system)
        if system != "Windows":
            profile_update = prepare_posix_path_update(install_dir, Path.home(), system)
    cmake = shutil.which("cmake")
    if cmake is None:
        raise InstallError("CMake is not on PATH; install CMake before running this script.")
    components = ["JoyeerRuntime"]
    if build.install_joypm:
        components.append("PRODUCT")
    for component in components:
        subprocess.run(
            [cmake, "--install", str(build.directory), "--config", "Debug",
             "--component", component, "--prefix", str(install_dir)],
            check=True,
        )
    for name in (*build.artifacts, "licenses/LICENSE"):
        require_file(install_dir / name, "installed artifact")
    for name, symbol in build.symbols.items():
        if symbol.is_dir():
            shutil.copytree(symbol, install_dir / name, dirs_exist_ok=True)
            require_dsym(install_dir / name)
        else:
            shutil.copy2(symbol, install_dir / name)
    if not args.skip_skill_install:
        for source, destination in skill_files:
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(source, destination)
        if skill_files:
            print(f"Joyeer skill installed to {skill_dir}")
        else:
            print(f"Joyeer skill already matches this checkout: {skill_dir}")
        print("Reload your agent session to discover the Joyeer skill.")
    if not args.skip_path_update:
        if system == "Windows":
            update_windows_path(install_dir)
        elif profile_update is not None:
            profile, content = profile_update
            if content is not None:
                profile.parent.mkdir(parents=True, exist_ok=True)
                profile.write_bytes(content)
            print(f"PATH setup is in {profile}. Open a new shell or reload this file.")
    print(f"Debug compiler installed to {install_dir}")
    print("This installation is for local testing only.")


def argument_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="Install an existing native Joyeer Debug build (Python 3.9+ and CMake required).",
        allow_abbrev=False,
    )
    parser.add_argument("--build-dir", type=Path, help="existing Debug build; defaults to the native Debug preset")
    parser.add_argument("--install-dir", type=Path, default=Path.home() / ".joyeer" / "bin",
                        help="binary destination (default: ~/.joyeer/bin)")
    parser.add_argument("--skip-path-update", action="store_true", help="leave PATH and shell profiles unchanged")
    parser.add_argument("--skill-dir", type=Path, default=Path.home() / ".agents" / "skills" / "joyeer",
                        help="full skill destination (default: ~/.agents/skills/joyeer)")
    parser.add_argument("--skip-skill-install", action="store_true", help="install binaries without the agent skill")
    parser.add_argument("--force", action="store_true", help="overwrite differing skill files; preserves extra files")
    return parser


def main(argv: Optional[list[str]] = None) -> int:
    args = argument_parser().parse_args(argv)
    try:
        install_debug(args, Path(__file__).resolve().parents[1], platform.system())
    except (InstallError, OSError, UnicodeError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    except subprocess.CalledProcessError as error:
        print(f"error: CMake installation failed with exit code {error.returncode}.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    sys.exit(main())
