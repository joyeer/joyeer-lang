from __future__ import annotations

from dataclasses import replace
import hashlib
import io
from pathlib import Path
import tarfile
import tempfile
import unittest

from scripts.toolchain_support.config import load_llvm_lock
from scripts.toolchain_support.llvm import (
    archive_is_valid,
    extract_source_archive,
    source_is_current,
)
from scripts.toolchain_support.paths import RepositoryPaths
from scripts.toolchain_support.process import ExecutionContext, ToolchainError


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
LOCK_PATH = REPOSITORY_ROOT / "third_party" / "llvm.lock.json"


def test_paths(root: Path) -> RepositoryPaths:
    deps = root / ".deps"
    return RepositoryPaths(
        root=root,
        deps=deps,
        downloads=deps / "downloads",
        sources=deps / "llvm-project",
        builds=deps / "llvm-build",
        installs=deps / "llvm-install",
        llvm_lock=LOCK_PATH,
    )


def create_archive(
    path: Path,
    unsafe_name: str | None = None,
    symlink: tuple[str, str] | None = None,
    hardlink: tuple[str, str] | None = None,
) -> bytes:
    with tarfile.open(path, mode="w:xz") as archive:
        directory = tarfile.TarInfo("llvm-project-test/llvm")
        directory.type = tarfile.DIRTYPE
        directory.mode = 0o755
        archive.addfile(directory)

        content = b"cmake_minimum_required(VERSION 3.16)\n"
        cmake = tarfile.TarInfo(unsafe_name or "llvm-project-test/llvm/CMakeLists.txt")
        cmake.size = len(content)
        cmake.mode = 0o644
        archive.addfile(cmake, io.BytesIO(content))
        if symlink is not None:
            link = tarfile.TarInfo(symlink[0])
            link.type = tarfile.SYMTYPE
            link.linkname = symlink[1]
            link.mode = 0o777
            archive.addfile(link)
        if hardlink is not None:
            link = tarfile.TarInfo(hardlink[0])
            link.type = tarfile.LNKTYPE
            link.linkname = hardlink[1]
            link.mode = 0o644
            archive.addfile(link)
    return path.read_bytes()


class LlvmArchiveTest(unittest.TestCase):
    def _lock_for_archive(self, archive: Path):
        lock = load_llvm_lock(LOCK_PATH)
        content = archive.read_bytes()
        return replace(
            lock,
            version="test",
            source=replace(
                lock.source,
                filename=archive.name,
                size=len(content),
                sha256=hashlib.sha256(content).hexdigest(),
            ),
        )

    def test_extracts_verified_archive_and_records_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "llvm.tar.xz"
            create_archive(archive)
            lock = self._lock_for_archive(archive)
            paths = test_paths(root)

            result = extract_source_archive(
                archive,
                lock,
                paths,
                force=False,
                context=ExecutionContext(),
            )

            self.assertTrue(archive_is_valid(archive, lock.source))
            self.assertTrue((result / "llvm" / "CMakeLists.txt").is_file())
            self.assertTrue(source_is_current(result, lock))

    def test_rejects_path_traversal(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "llvm.tar.xz"
            create_archive(archive, "../escape")
            lock = self._lock_for_archive(archive)

            with self.assertRaisesRegex(ToolchainError, "unsafe path"):
                extract_source_archive(
                    archive,
                    lock,
                    test_paths(root),
                    force=False,
                    context=ExecutionContext(),
                )
            self.assertFalse((root / "escape").exists())

    def test_extracts_relative_symlink_within_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "llvm.tar.xz"
            create_archive(
                archive,
                symlink=(
                    "llvm-project-test/llvm/CMakeLists.link",
                    "CMakeLists.txt",
                ),
            )
            lock = self._lock_for_archive(archive)

            result = extract_source_archive(
                archive,
                lock,
                test_paths(root),
                force=False,
                context=ExecutionContext(),
            )

            link = result / "llvm" / "CMakeLists.link"
            self.assertTrue(link.is_symlink())
            self.assertEqual(link.readlink(), Path("CMakeLists.txt"))

    def test_rejects_symlink_that_escapes_archive(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "llvm.tar.xz"
            create_archive(
                archive,
                symlink=("llvm-project-test/escape", "../../outside"),
            )
            lock = self._lock_for_archive(archive)

            with self.assertRaisesRegex(ToolchainError, "escapes the archive"):
                extract_source_archive(
                    archive,
                    lock,
                    test_paths(root),
                    force=False,
                    context=ExecutionContext(),
                )
            self.assertFalse((root / "outside").exists())

    def test_extracts_hardlink_to_regular_archive_file(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            archive = root / "llvm.tar.xz"
            create_archive(
                archive,
                hardlink=(
                    "llvm-project-test/llvm/CMakeLists.copy",
                    "llvm-project-test/llvm/CMakeLists.txt",
                ),
            )
            lock = self._lock_for_archive(archive)

            result = extract_source_archive(
                archive,
                lock,
                test_paths(root),
                force=False,
                context=ExecutionContext(),
            )

            original = result / "llvm" / "CMakeLists.txt"
            linked = result / "llvm" / "CMakeLists.copy"
            self.assertTrue(original.samefile(linked))


if __name__ == "__main__":
    unittest.main()