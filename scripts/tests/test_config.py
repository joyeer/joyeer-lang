from __future__ import annotations

import json
from pathlib import Path
import tempfile
import unittest

from scripts.toolchain_support.config import LockError, load_llvm_lock


REPOSITORY_ROOT = Path(__file__).resolve().parents[2]
LOCK_PATH = REPOSITORY_ROOT / "third_party" / "llvm.lock.json"


class LlvmLockTest(unittest.TestCase):
    def test_loads_pinned_llvm_release(self) -> None:
        lock = load_llvm_lock(LOCK_PATH)

        self.assertEqual(lock.version, "22.1.8")
        self.assertEqual(lock.commit, "ca7933e47d3a3451d81e72ac174dcb5aa28b59d1")
        self.assertEqual(
            lock.source.sha256,
            "922f1817a0df7b1489272d18134ee0087a8b068828f87ac63b9861b1a9965888",
        )
        self.assertEqual(lock.targets_for("macos-arm64"), ("AArch64",))
        self.assertEqual(len(lock.fingerprint), 64)

    def test_rejects_unsupported_schema(self) -> None:
        value = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
        value["schema_version"] = 99
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "llvm.lock.json"
            path.write_text(json.dumps(value), encoding="utf-8")

            with self.assertRaisesRegex(LockError, "unsupported LLVM lock schema"):
                load_llvm_lock(path)

    def test_rejects_non_https_source(self) -> None:
        value = json.loads(LOCK_PATH.read_text(encoding="utf-8"))
        value["llvm"]["source"]["url"] = "http://example.invalid/llvm.tar.xz"
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "llvm.lock.json"
            path.write_text(json.dumps(value), encoding="utf-8")

            with self.assertRaisesRegex(LockError, "must be an HTTPS URL"):
                load_llvm_lock(path)


if __name__ == "__main__":
    unittest.main()