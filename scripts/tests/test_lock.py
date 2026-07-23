from __future__ import annotations

from pathlib import Path
import tempfile
import unittest

from scripts.toolchain_support.lock import FileLock
from scripts.toolchain_support.process import ToolchainError


class FileLockTest(unittest.TestCase):
    def test_rejects_concurrent_owner_and_releases_lock(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "toolchain.lock"

            with FileLock(path):
                with self.assertRaisesRegex(ToolchainError, "another toolchain process"):
                    with FileLock(path):
                        self.fail("concurrent lock unexpectedly succeeded")

            with FileLock(path):
                pass


if __name__ == "__main__":
    unittest.main()