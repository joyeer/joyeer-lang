from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import tarfile
import tempfile
from typing import BinaryIO, Dict, Optional, Tuple
import urllib.error
import urllib.request

from .config import LlvmLock, SourceArchive
from .lock import FileLock
from .paths import RepositoryPaths
from .process import ExecutionContext, ToolchainError


SOURCE_MARKER = ".joyeer-source.json"
DOWNLOAD_CHUNK_SIZE = 1024 * 1024


def _sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(DOWNLOAD_CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def archive_is_valid(path: Path, source: SourceArchive) -> bool:
    try:
        return path.stat().st_size == source.size and _sha256(path) == source.sha256
    except OSError:
        return False


def _proxy_names() -> Tuple[str, ...]:
    names = []
    for name in ("https_proxy", "http_proxy", "all_proxy"):
        if os.environ.get(name) or os.environ.get(name.upper()):
            names.append(name)
    return tuple(names)


def _copy_response(response: BinaryIO, output: BinaryIO, expected_size: int) -> int:
    written = 0
    next_progress = 10
    while True:
        chunk = response.read(DOWNLOAD_CHUNK_SIZE)
        if not chunk:
            return written
        output.write(chunk)
        written += len(chunk)
        progress = written * 100 // expected_size
        if progress >= next_progress:
            print(f"Downloaded {min(progress, 100)}%", flush=True)
            next_progress += 10


def download_source_archive(
    lock: LlvmLock,
    paths: RepositoryPaths,
    *,
    offline: bool,
    force: bool,
    context: ExecutionContext,
) -> Path:
    destination = paths.downloads / lock.source.filename
    if not force and archive_is_valid(destination, lock.source):
        context.log(f"Using verified LLVM archive: {destination}")
        return destination
    if offline:
        raise ToolchainError(
            f"offline mode requires a verified LLVM archive at '{destination}'"
        )
    if context.dry_run:
        context.log(f"Would download {lock.source.url} to {destination}")
        return destination

    paths.downloads.mkdir(parents=True, exist_ok=True)
    proxy_names = _proxy_names()
    if proxy_names:
        context.log("Using proxy settings from: " + ", ".join(proxy_names))
    request = urllib.request.Request(
        lock.source.url,
        headers={"User-Agent": "Joyeer-toolchain-bootstrap/1"},
    )
    temporary_path: Optional[Path] = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w+b",
            prefix=f".{lock.source.filename}.",
            suffix=".tmp",
            dir=paths.downloads,
            delete=False,
        ) as output:
            temporary_path = Path(output.name)
            print(f"Downloading LLVM {lock.version} ({lock.source.size} bytes)")
            with urllib.request.urlopen(request, timeout=60) as response:
                written = _copy_response(response, output, lock.source.size)
            output.flush()
            os.fsync(output.fileno())
        if written != lock.source.size:
            raise ToolchainError(
                f"LLVM archive size mismatch: expected {lock.source.size}, received {written}"
            )
        if _sha256(temporary_path) != lock.source.sha256:
            raise ToolchainError("LLVM archive SHA-256 does not match llvm.lock.json")
        os.replace(temporary_path, destination)
        temporary_path = None
        return destination
    except (urllib.error.URLError, TimeoutError) as error:
        raise ToolchainError(f"cannot download LLVM source: {error}") from error
    except OSError as error:
        raise ToolchainError(f"cannot store LLVM source archive: {error}") from error
    finally:
        if temporary_path is not None:
            try:
                temporary_path.unlink()
            except FileNotFoundError:
                pass


def _safe_path_parts(value: str, description: str) -> Tuple[str, ...]:
    if "\\" in value:
        raise ToolchainError(f"{description} uses a backslash path: {value}")
    path = PurePosixPath(value)
    if path.is_absolute() or not path.parts:
        raise ToolchainError(f"{description} has an unsafe path: {value}")
    parts = tuple(part for part in path.parts if part != ".")
    if not parts or any(part in ("", "..") or ":" in part for part in parts):
        raise ToolchainError(f"{description} has an unsafe path: {value}")
    return parts


def _safe_member_parts(member: tarfile.TarInfo) -> Tuple[str, ...]:
    parts = _safe_path_parts(member.name, "LLVM archive member")
    if not (member.isdir() or member.isfile() or member.issym() or member.islnk()):
        raise ToolchainError(
            f"LLVM archive contains an unsupported special file: {member.name}"
        )
    return parts


def _normalize_relative_link(
    member_parts: Tuple[str, ...],
    link_name: str,
    top_level: str,
) -> Tuple[str, ...]:
    if "\\" in link_name:
        raise ToolchainError(
            f"LLVM archive link uses a backslash target: {'/'.join(member_parts)}"
        )
    link_path = PurePosixPath(link_name)
    if link_path.is_absolute() or not link_path.parts:
        raise ToolchainError(
            f"LLVM archive link has an unsafe target: {'/'.join(member_parts)}"
        )
    result = list(member_parts[:-1])
    for part in link_path.parts:
        if part in ("", "."):
            continue
        if part == "..":
            if not result:
                raise ToolchainError(
                    f"LLVM archive link escapes the archive: {'/'.join(member_parts)}"
                )
            result.pop()
            continue
        if ":" in part:
            raise ToolchainError(
                f"LLVM archive link has an unsafe target: {'/'.join(member_parts)}"
            )
        result.append(part)
    if not result or result[0] != top_level:
        raise ToolchainError(
            f"LLVM archive link escapes the archive: {'/'.join(member_parts)}"
        )
    return tuple(result)


def _normalize_hardlink(
    member_parts: Tuple[str, ...],
    link_name: str,
    top_level: str,
) -> Tuple[str, ...]:
    target_parts = _safe_path_parts(link_name, "LLVM archive hardlink target")
    if target_parts[0] != top_level:
        raise ToolchainError(
            f"LLVM archive link escapes the archive: {'/'.join(member_parts)}"
        )
    return target_parts


def _write_archive_file(
    archive: tarfile.TarFile,
    member: tarfile.TarInfo,
    target: Path,
) -> None:
    source = archive.extractfile(member)
    if source is None:
        raise ToolchainError(f"cannot read LLVM archive member: {member.name}")
    target.parent.mkdir(parents=True, exist_ok=True)
    with source, target.open("xb") as output:
        shutil.copyfileobj(source, output, length=DOWNLOAD_CHUNK_SIZE)
    target.chmod(stat.S_IMODE(member.mode))


def _marker_value(lock: LlvmLock) -> Dict[str, object]:
    return {
        "schema_version": 1,
        "lock_fingerprint": lock.fingerprint,
        "llvm_version": lock.version,
        "llvm_commit": lock.commit,
        "archive_sha256": lock.source.sha256,
    }


def source_is_current(path: Path, lock: LlvmLock) -> bool:
    marker = path / SOURCE_MARKER
    try:
        value = json.loads(marker.read_text(encoding="utf-8"))
    except (OSError, json.JSONDecodeError):
        return False
    return value == _marker_value(lock) and (path / "llvm" / "CMakeLists.txt").is_file()


def extract_source_archive(
    archive_path: Path,
    lock: LlvmLock,
    paths: RepositoryPaths,
    *,
    force: bool,
    context: ExecutionContext,
) -> Path:
    destination = paths.sources / lock.version
    if source_is_current(destination, lock) and not force:
        context.log(f"Using extracted LLVM source: {destination}")
        return destination
    if destination.exists() and not force:
        raise ToolchainError(
            f"LLVM source at '{destination}' does not match the lock; retry with --force"
        )
    if context.dry_run:
        context.log(f"Would extract {archive_path} to {destination}")
        return destination
    if not archive_is_valid(archive_path, lock.source):
        raise ToolchainError(f"LLVM archive is not valid: '{archive_path}'")

    paths.sources.mkdir(parents=True, exist_ok=True)
    temporary_root = Path(
        tempfile.mkdtemp(prefix=f".{lock.version}.", dir=paths.sources)
    )
    payload = temporary_root / "payload"
    top_level: Optional[str] = None
    directory_modes = []
    links = []
    try:
        with tarfile.open(archive_path, mode="r:xz") as archive:
            for member in archive:
                parts = _safe_member_parts(member)
                if top_level is None:
                    top_level = parts[0]
                elif parts[0] != top_level:
                    raise ToolchainError("LLVM archive must contain one top-level directory")
                target = payload.joinpath(*parts)
                if member.isdir():
                    target.mkdir(parents=True, exist_ok=True)
                    directory_modes.append((target, stat.S_IMODE(member.mode)))
                elif member.isfile():
                    _write_archive_file(archive, member, target)
                else:
                    links.append((member, parts, target))
        if top_level is None:
            raise ToolchainError("LLVM archive is empty")
        for member, parts, target in links:
            target.parent.mkdir(parents=True, exist_ok=True)
            if member.issym():
                _normalize_relative_link(parts, member.linkname, top_level)
                try:
                    target.symlink_to(member.linkname)
                except OSError as error:
                    guidance = (
                        " Enable Windows Developer Mode or run with permission to create "
                        "symbolic links."
                        if os.name == "nt"
                        else ""
                    )
                    raise ToolchainError(
                        f"cannot create LLVM archive symlink '{member.name}': {error}."
                        f"{guidance}"
                    ) from error
            else:
                target_parts = _normalize_hardlink(parts, member.linkname, top_level)
                hardlink_target = payload.joinpath(*target_parts)
                if not hardlink_target.is_file() or hardlink_target.is_symlink():
                    raise ToolchainError(
                        f"LLVM archive hardlink target is not a regular file: {member.linkname}"
                    )
                os.link(hardlink_target, target)
        extracted = payload / top_level
        if not (extracted / "llvm" / "CMakeLists.txt").is_file():
            raise ToolchainError("LLVM archive does not contain llvm/CMakeLists.txt")
        for directory, mode in reversed(directory_modes):
            directory.chmod(mode)
        (extracted / SOURCE_MARKER).write_text(
            json.dumps(_marker_value(lock), indent=2, sort_keys=True) + "\n",
            encoding="utf-8",
        )
        if destination.exists():
            shutil.rmtree(destination)
        os.replace(extracted, destination)
        return destination
    except (OSError, tarfile.TarError) as error:
        raise ToolchainError(f"cannot extract LLVM source: {error}") from error
    finally:
        shutil.rmtree(temporary_root, ignore_errors=True)


def fetch_llvm(
    lock: LlvmLock,
    paths: RepositoryPaths,
    *,
    offline: bool,
    force: bool,
    archive_only: bool,
    context: ExecutionContext,
) -> Path:
    def fetch() -> Path:
        archive = download_source_archive(
            lock,
            paths,
            offline=offline,
            force=force,
            context=context,
        )
        if archive_only:
            return archive
        return extract_source_archive(
            archive,
            lock,
            paths,
            force=force,
            context=context,
        )

    if context.dry_run:
        return fetch()
    with FileLock(paths.deps / "locks" / "llvm.lock"):
        return fetch()