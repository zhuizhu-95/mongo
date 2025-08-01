"""Test resmoke's integration with mocha-style style tests."""

import re
import subprocess
import sys
import unittest


def execute_resmoke(resmoke_args):
    return subprocess.run(
        [sys.executable, "buildscripts/resmoke.py", "run"] + resmoke_args,
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )


class TestMochaRunner(unittest.TestCase):
    def test_mocha_runner(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_mocha_runner.yml",
            "jstests/noPassthrough/shell/js/mochalite.js",
        ]

        result = execute_resmoke(resmoke_args)

        self.assertEqual(result.returncode, 0)

        for output in [
            "✔ test0",
            "✔ test1",
            "✔ describeA > test2",
            "✔ describeA > test3",
            "✔ describeA > describeB > test4",
            "✔ describeA > describeB > test5",
            "✔ describeA > test6",
            "✔ describeA > test7",
            "✔ test8",
        ]:
            self.assertIn(output, result.stdout)

    def test_mocha_runner_hooks(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_mocha_runner.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/mocha/hooks.js",
        ]

        result = execute_resmoke(resmoke_args)

        self.assertEqual(result.returncode, 0)

        arr = [
            "before1",
            "before2",
            "--beforeEach1",
            "--beforeEach2",
            "----test1",
            "--afterEach1",
            "--afterEach2",
            "--beforeEach1",
            "--beforeEach2",
            "----test2",
            "--afterEach1",
            "--afterEach2",
            "----describe before1",
            "----describe before2",
            "--beforeEach1",
            "--beforeEach2",
            "------describe beforeEach1",
            "------describe beforeEach2",
            "--------test3",
            "------describe afterEach1",
            "------describe afterEach2",
            "--afterEach1",
            "--afterEach2",
            "--beforeEach1",
            "--beforeEach2",
            "------describe beforeEach1",
            "------describe beforeEach2",
            "--------test4",
            "------describe afterEach1",
            "------describe afterEach2",
            "--afterEach1",
            "--afterEach2",
            "----describe after1",
            "----describe after2",
            "--beforeEach1",
            "--beforeEach2",
            "----test5",
            "--afterEach1",
            "--afterEach2",
            "--beforeEach1",
            "--beforeEach2",
            "----test6",
            "--afterEach1",
            "--afterEach2",
            "after1",
            "after2",
        ]
        for output in arr:
            self.assertIn(output, result.stdout)

        # verify ordering
        pattern = r".*".join(arr) + r".*"
        pattern = re.compile(pattern, re.DOTALL)
        self.assertRegex(result.stdout, pattern)

    def test_mocha_runner_async(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_mocha_runner.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/mocha/async.js",
        ]

        result = execute_resmoke(resmoke_args)

        self.assertEqual(result.returncode, 0)

        arr = [
            "before1",
            "before2",
            "--beforeEach1",
            "--beforeEach2",
            "----test1",
            "--afterEach1",
            "--afterEach2",
            "--beforeEach1",
            "--beforeEach2",
            "----test2",
            "--afterEach1",
            "--afterEach2",
            "----describe before1",
            "----describe before2",
            "--beforeEach1",
            "--beforeEach2",
            "------describe beforeEach1",
            "------describe beforeEach2",
            "--------test3",
            "------describe afterEach1",
            "------describe afterEach2",
            "--afterEach1",
            "--afterEach2",
            "--beforeEach1",
            "--beforeEach2",
            "------describe beforeEach1",
            "------describe beforeEach2",
            "--------test4",
            "------describe afterEach1",
            "------describe afterEach2",
            "--afterEach1",
            "--afterEach2",
            "----describe after1",
            "----describe after2",
            "--beforeEach1",
            "--beforeEach2",
            "----test5",
            "--afterEach1",
            "--afterEach2",
            "--beforeEach1",
            "--beforeEach2",
            "----test6",
            "--afterEach1",
            "--afterEach2",
            "after1",
            "after2",
        ]
        for output in arr:
            self.assertIn(output, result.stdout)

        # verify ordering
        pattern = r".*".join(arr) + r".*"
        pattern = re.compile(pattern, re.DOTALL)
        self.assertRegex(result.stdout, pattern)

    def test_mocha_runner_fail(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_mocha_runner.yml",
            "buildscripts/tests/resmoke_end2end/testfiles/mocha/test_fail.js",
        ]

        result = execute_resmoke(resmoke_args)

        self.assertNotEqual(result.returncode, 0)

        for output in [
            "✔ test1",
            "✔ test2",
            "✔ describe > test3",
            "\x1b[31m✘ describe > test4\x1b[0m",
        ]:
            self.assertIn(output, result.stdout)

        # hard assertion halts the runner, does not reach test5
        self.assertNotIn("✔ describe > test5", result.stdout)

    def test_mocha_runner_grep(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_mocha_runner.yml",
            "--mochagrep="
            + "|".join(
                [
                    "ana",  # partial match
                    "tomato",  # match across describe suites
                    "chicken",  # no match
                    "^vegetables > spinach$",  # full exact match
                ]
            ),
            "buildscripts/tests/resmoke_end2end/testfiles/mocha/grep_names.js",
        ]

        result = execute_resmoke(resmoke_args)

        self.assertEqual(result.returncode, 0)

        arr = [
            "✔ fruits > banana",
            "✔ fruits > tomato",
            "✔ vegetables > spinach",
            "✔ vegetables > tomato",
        ]
        for output in arr:
            self.assertIn(output, result.stdout)

        # verify ordering
        pattern = r".*".join(arr) + r".*"
        pattern = re.compile(pattern, re.DOTALL)
        self.assertRegex(result.stdout, pattern)


    def test_mocha_runner_only(self):
        resmoke_args = [
            "--suites=buildscripts/tests/resmoke_end2end/suites/resmoke_selftest_mocha_runner.yml",
            "jstests/noPassthrough/shell/js/mochalite_only.js",
        ]

        result = execute_resmoke(resmoke_args)

        self.assertEqual(result.returncode, 0)

        arr = [
            "test4",
            "test9",
            "test11",
            "test13",
        ]
        for output in arr:
            self.assertIn(output, result.stdout)

        # verify ordering
        pattern = r".*".join(arr) + r".*"
        pattern = re.compile(pattern, re.DOTALL)
        self.assertRegex(result.stdout, pattern)
