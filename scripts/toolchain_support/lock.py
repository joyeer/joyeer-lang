from __future__ import annotations

import os
from pathlib import Path
from typing import BinaryIO, Optional

from .process import ToolchainError


class FileLock:
    """A non-blocking advisory lock released automatically when the process exits."""

    def __init__(self, path: Path) -> None:
        self.path = path
        self._file: Optional[BinaryIO] = None

    def __enter__(self) -> "FileLock":
        self.path.parent.mkdir(parents=True, exist_ok=True)
        lock_file = self.path.open("a+b")
        try:
            if os.name == "nt":
                import msvcrt

                lock_file.seek(0, os.SEEK_END)
                if lock_file.tell() == 0:
                    lock_file.write(b"\0")
                    lock_file.flush()
                lock_file.seek(0)
                msvcrt.locking(lock_file.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl

                fcntl.flock(lock_file.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as error:
            lock_file.close()
            raise ToolchainError(
                f"another toolchain process is using '{self.path.parent}'"
            ) from error
        self._file = lock_file
        return self

    def __exit__(self, exception_type, exception, traceback) -> None:
        if self._file is None:
            return
        try:
            if os.name == "nt":
                import msvcrt

                self._file.seek(0)
                msvcrt.locking(self._file.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl

                fcntl.flock(self._file.fileno(), fcntl.LOCK_UN)
        finally:
            self._file.close()
            self._file = None