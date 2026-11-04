import contextlib
import io
from pathlib import Path
import sys
from types import SimpleNamespace
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "scripts"))
import validate


class ValidationSummaryTests(unittest.TestCase):
    def test_success_with_both_ctest_summary_formats(self):
        for output in ("100% tests passed out of 3",
                       "100% tests passed, 0 tests failed out of 3"):
            with self.subTest(output=output):
                self.assertEqual(validate.parse_ctest(output), (True, "3 / 3 passed", []))

    def test_skipped_tests_are_not_counted_as_passes(self):
        output = (
            "100% tests passed, 0 tests failed out of 3\n"
            "The following tests did not run:\n"
            "\t2 - Filesystem.InvalidBytes (Skipped)\n"
        )
        self.assertEqual(validate.parse_ctest(output), (True, "2 / 3 passed; 1 skipped", []))

    def test_all_skipped_is_not_a_successful_suite(self):
        output = "100% tests passed out of 1\n\t1 - Unavailable (Skipped)\n"
        self.assertEqual(validate.parse_ctest(output), (False, "0 / 1 passed; 1 skipped", []))

    def test_failed_and_skipped_tests_are_distinguished(self):
        output = (
            "67% tests passed, 1 tests failed out of 3\n"
            "The following tests did not run:\n"
            "\t2 - Unavailable (Skipped)\n\n"
            "The following tests FAILED:\n"
            "\t3 - Broken (Failed)\n"
        )
        self.assertEqual(validate.parse_ctest(output),
                         (False, "1 / 3 passed; 1 skipped", ["Broken (Failed)"]))

    def test_empty_or_missing_ctest_summary_fails(self):
        self.assertFalse(validate.parse_ctest("No tests were found!!!")[0])
        self.assertFalse(validate.parse_ctest("100% tests passed out of 0")[0])

    def run_with_retry(self, initial, rerun):
        args = SimpleNamespace(
            fresh=False, no_compression=False, define=[], tests_list=None,
            skip_ctest=False, jobs=2, no_retry=False, skip_fern=True,
        )
        with patch.object(validate, "run", side_effect=[
            (0, ""), (0, ""), (8, initial), (0, rerun),
        ]), contextlib.redirect_stdout(io.StringIO()):
            return validate.validate_config("Debug", args, Path("sdk"), {})[-1]

    def test_retry_preserves_original_skips(self):
        failed = (
            "67% tests passed, 1 tests failed out of 3\n"
            "\t2 - Unavailable (Skipped)\n"
            "The following tests FAILED:\n\t3 - Flaky (Failed)\n"
        )
        row = self.run_with_retry(failed, "100% tests passed out of 1")
        self.assertTrue(row.ok)
        self.assertEqual(row.summary,
                         "2 / 3 passed; 1 skipped; 1 passed only on rerun: Flaky")

    def test_skipping_a_failed_test_on_retry_does_not_hide_the_failure(self):
        failed = (
            "0% tests passed, 2 tests failed out of 2\n"
            "The following tests FAILED:\n"
            "\t1 - Broken (Failed)\n\t2 - Flaky (Failed)\n"
        )
        rerun = "100% tests passed out of 2\n\t1 - Broken (Skipped)\n"
        row = self.run_with_retry(failed, rerun)
        self.assertFalse(row.ok)
        self.assertIn("Broken (Failed)", row.summary)
        self.assertNotIn("passed only on rerun", row.summary)

    def test_fern_counts_must_agree(self):
        self.assertEqual(validate.parse_fern("2 tests: 2 passed, 0 build-fail, 0 run-fail"),
                         (True, "2 / 2 passed"))
        for output in ("2 tests: 1 passed, 0 build-fail, 0 run-fail",
                       "2 tests: 1 passed, 1 build-fail, 0 run-fail",
                       "0 tests: 0 passed, 0 build-fail, 0 run-fail",
                       ""):
            with self.subTest(output=output):
                self.assertFalse(validate.parse_fern(output)[0])


if __name__ == "__main__":
    unittest.main()
