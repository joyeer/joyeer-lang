from __future__ import annotations

import unittest

from scripts.toolchain_support.process import ToolchainError
from scripts.toolchain_support.targets import normalize_host


class HostTest(unittest.TestCase):
    def test_normalizes_supported_hosts(self) -> None:
        self.assertEqual(normalize_host("Darwin", "arm64").key, "macos-arm64")
        self.assertEqual(normalize_host("Windows", "AMD64").key, "windows-x86_64")
        self.assertEqual(normalize_host("Linux", "aarch64").key, "linux-arm64")

    def test_rejects_unknown_host(self) -> None:
        with self.assertRaisesRegex(ToolchainError, "unsupported host platform"):
            normalize_host("Plan9", "mips")


if __name__ == "__main__":
    unittest.main()