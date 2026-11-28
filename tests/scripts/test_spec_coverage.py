from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).resolve().parents[2] / "scripts" / "spec-coverage.py"


class SpecCoverageTests(unittest.TestCase):
    def report(self, citation):
        with tempfile.TemporaryDirectory() as work:
            root = Path(work)
            (root / "specs").mkdir()
            (root / "specs" / "04-memory.md").write_text(
                "## \u00a74 Memory\n### 4.1 Values\n### 4.2 Access\n", encoding="utf-8")
            (root / "tests").mkdir()
            (root / "tests" / "sample.joyeer").write_text(citation + "\n", encoding="utf-8")
            return subprocess.run([sys.executable, "-X", "utf8", str(SCRIPT), "--root", str(root)],
                                  capture_output=True, encoding="utf-8", timeout=10)

    def test_exact_section_only(self):
        result = self.report("// spec: 04-memory.md \u00a74.2")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("04-memory.md \u00a74.1", result.stdout)
        self.assertNotIn("04-memory.md \u00a74.2", result.stdout)
        self.assertIn("1 of 3 sections", result.stdout)

    def test_invalid_citation_is_rejected(self):
        result = self.report("// spec: 04-memory.md \u00a74.99")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("sample.joyeer:1: invalid spec citation", result.stderr)


if __name__ == "__main__":
    unittest.main()
