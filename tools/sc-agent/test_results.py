"""Tests for test/build command detection and output summaries (results.py). Stdlib only."""

import sys

sys.dont_write_bytecode = True

import unittest  # noqa: E402

from results import CommandDetector, Outcome, exit_code_from_error, summarize  # noqa: E402


class DetectionTest(unittest.TestCase):
    def setUp(self) -> None:
        self.detector = CommandDetector()

    def test_test_commands(self):
        for command in (
            "pytest -q",
            "python -m pytest tests/test_x.py -x",
            "python3 -m unittest discover -s tools/sc-agent -v",
            ".venv/bin/python -m pytest",
            "py -m pytest",
            "tox -e py312",
            "nox -s tests",
            "cd build && ctest --output-on-failure",
            "make test",
            "time make -C build check",
            "npm test",
            "npm run test:unit",
            "pnpm test",
            "yarn test",
            "npx jest --ci",
            "yarn vitest run",
            "pnpm exec vitest",
            "cargo test --workspace",
            "go test ./...",
            "mvn -q test",
            "./gradlew test",
            "dotnet test",
            "FOO=1 BAR='a b' uv run pytest",
            "poetry run python -m pytest",
            "source .venv/bin/activate && pytest",
            "pytest 2>&1 | tail -20",
            "echo start\npytest -q",
            "cmake --build build && ctest",  # test wins over build
        ):
            with self.subTest(command=command):
                self.assertEqual(self.detector.kind(command), "test")

    def test_build_commands(self):
        for command in (
            "cmake --build build -j8",
            "ninja -C build",
            "cargo build --release",
            "go build ./cmd/x",
            "idf.py build",
            "idf.py -p COM12 flash monitor",
            "(cd firmware && idf.py app-flash)",
        ):
            with self.subTest(command=command):
                self.assertEqual(self.detector.kind(command), "build")

    def test_other_commands(self):
        for command in (
            "ls -la",
            "git commit -m \"fix; pytest\"",
            "test -f x && echo yes",
            "make",
            "npm install",
            "echo pytest",
            "cat test_results.py",
            "",
            None,
        ):
            with self.subTest(command=command):
                self.assertIsNone(self.detector.kind(command))

    def test_extra_patterns(self):
        detector = CommandDetector([r"\./run_tests\.sh\b", r"bats\b"])
        self.assertEqual(detector.kind("./run_tests.sh --fast"), "test")
        self.assertEqual(detector.kind("FOO=1 bats test/"), "test")
        self.assertIsNone(self.detector.kind("./run_tests.sh"))


PYTEST_FAIL = """\
============================= test session starts ==============================
collected 45 items

tests/test_a.py ..F.....                                                 [ 17%]
tests/test_b.py .......F.F....                                           [ 48%]
=========================== short test summary info ============================
FAILED tests/test_a.py::test_x - AssertionError
FAILED tests/test_b.py::test_y - KeyError: 'k'
FAILED tests/test_b.py::test_z - ValueError
=================== 3 failed, 42 passed, 1 skipped in 1.23s ====================
"""

UNITTEST_FAIL = """\
F.....E.
======================================================================
FAIL: test_x (test_mod.T.test_x)
----------------------------------------------------------------------
Traceback (most recent call last):
AssertionError: 1 != 2

----------------------------------------------------------------------
Ran 64 tests in 9.313s

FAILED (failures=2, errors=1, skipped=3)
"""

CTEST_FAIL = """\
    Start 1: unit
1/4 Test #1: unit .............................   Passed    0.01 sec
4/4 Test #4: slow .............................***Failed    2.00 sec

75% tests passed, 1 tests failed out of 4

Total Test time (real) =   2.05 sec
"""

JEST_FAIL = """\
FAIL src/app.test.ts
  x renders (12 ms)

Test Suites: 1 failed, 4 passed, 5 total
Tests:       1 failed, 41 passed, 42 total
Snapshots:   0 total
Time:        3.21 s
"""

VITEST_PASS = """\
 ✓ src/a.test.ts (3 tests) 4ms
 ✓ src/b.test.ts (39 tests) 20ms

 Test Files  2 passed (2)
      Tests  42 passed (42)
   Start at  10:00:00
   Duration  1.2s
"""

CARGO_FAIL = """\
running 10 tests
test result: ok. 10 passed; 0 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.01s

running 5 tests
test b ... FAILED
test result: FAILED. 3 passed; 2 failed; 0 ignored; 0 measured; 0 filtered out; finished in 0.02s
"""

GO_FAIL = """\
ok  \texample.com/x/a\t0.012s
?   \texample.com/x/cmd\t[no test files]
--- FAIL: TestB (0.00s)
    b_test.go:10: want 1, got 2
FAIL
FAIL\texample.com/x/b\t0.020s
ok  \texample.com/x/c\t(cached)
FAIL
"""


class SummaryTest(unittest.TestCase):
    def test_pytest(self):
        self.assertEqual(summarize(PYTEST_FAIL, 1, "test"), Outcome(False, "3 failed"))
        self.assertEqual(summarize("....\n42 passed in 0.12s\n", 0, "test"), Outcome(True, "42 passed"))
        self.assertEqual(summarize("== 1 error, 4 passed in 0.5s ==", 2, "test"), Outcome(False, "1 failed"))
        self.assertEqual(summarize("== 2 passed, 1 xfailed, 1 xpassed in 0.5s (0:00:00) ==", 0, "test"),
                         Outcome(True, "2 passed"))

    def test_unittest(self):
        self.assertEqual(summarize(UNITTEST_FAIL, 1, "test"), Outcome(False, "3 failed"))
        self.assertEqual(summarize("....\nRan 4 tests in 0.010s\n\nOK (skipped=1)\n", 0, "test"),
                         Outcome(True, "3 passed"))

    def test_ctest(self):
        self.assertEqual(summarize(CTEST_FAIL, 8, "test"), Outcome(False, "1 failed"))
        ok = "100% tests passed, 0 tests failed out of 12"
        self.assertEqual(summarize(ok, 0, "test"), Outcome(True, "12 passed"))

    def test_jest_and_vitest(self):
        self.assertEqual(summarize(JEST_FAIL, 1, "test"), Outcome(False, "1 failed"))
        self.assertEqual(summarize(VITEST_PASS, 0, "test"), Outcome(True, "42 passed"))
        self.assertEqual(summarize("      Tests  3 failed | 39 passed (42)", 1, "test"), Outcome(False, "3 failed"))

    def test_cargo_sums_all_test_binaries(self):
        self.assertEqual(summarize(CARGO_FAIL, 101, "test"), Outcome(False, "2 failed"))
        ok = CARGO_FAIL.replace("3 passed; 2 failed", "5 passed; 0 failed").replace("FAILED.", "ok.")
        self.assertEqual(summarize(ok, 0, "test"), Outcome(True, "15 passed"))

    def test_go_counts_packages(self):
        self.assertEqual(summarize(GO_FAIL, 1, "test"), Outcome(False, "1 failed"))
        self.assertEqual(summarize("ok  \texample.com/x/a\t0.01s\nok  \texample.com/x/b\t(cached)\n", 0, "test"),
                         Outcome(True, "2 passed"))

    def test_idf(self):
        build = "[1024/1024] Linking CXX executable stack-chan.elf\nProject build complete. To flash, run:\n"
        self.assertEqual(summarize(build, 0, "build"), Outcome(True, "build ok"))
        failed = "ninja: build stopped: subcommand failed."
        self.assertEqual(summarize(failed, 2, "build"), Outcome(False, "build failed"))
        flash = build + "A fatal error occurred: Could not open COM12, the port is busy"
        self.assertEqual(summarize(flash, 2, "build"), Outcome(False, "flash failed"))

    def test_fallback_by_exit_code(self):
        self.assertEqual(summarize("all good", 0, "test"), Outcome(True, "tests ok"))
        self.assertEqual(summarize("boom", 1, "test"), Outcome(False, "tests failed"))
        self.assertEqual(summarize("", 0, "build"), Outcome(True, "build ok"))
        self.assertEqual(summarize("", 0, "run"), Outcome(True, "ok"))
        self.assertEqual(summarize("oops", 2, "run"), Outcome(False, "exit 2"))
        self.assertEqual(summarize(None, 0, "run"), Outcome(True, "ok"))

    def test_counts_with_failing_exit_code_but_no_failures(self):
        # e.g. a coverage threshold failed after all tests passed
        self.assertEqual(summarize("10 passed in 1.0s\nFAIL Required test coverage of 90% not reached", 1, "test"),
                         Outcome(False, "tests failed"))

    def test_last_summary_wins(self):
        output = "Ran 3 tests in 0.1s\n\nOK\n" + PYTEST_FAIL
        self.assertEqual(summarize(output, 1, "test"), Outcome(False, "3 failed"))
        self.assertEqual(summarize(PYTEST_FAIL + "Ran 3 tests in 0.1s\n\nOK\n", 0, "test"), Outcome(True, "3 passed"))

    def test_ansi_and_crlf(self):
        output = "\x1b[32m\x1b[1m===== 5 passed in 0.20s =====\x1b[0m\r\n"
        self.assertEqual(summarize(output, 0, "test"), Outcome(True, "5 passed"))
        self.assertEqual(summarize("Ran 2 tests in 0.0s\r\n\r\nOK\r\n", 0, "test"), Outcome(True, "2 passed"))

    def test_exit_code_from_error(self):
        self.assertEqual(exit_code_from_error("Exit code 1\nError: Cannot find module 'express'"), 1)
        self.assertEqual(exit_code_from_error("Exit code 130"), 130)
        self.assertIsNone(exit_code_from_error("Command timed out after 2m"))
        self.assertIsNone(exit_code_from_error(None))


if __name__ == "__main__":
    unittest.main()
