#!/usr/bin/env python3
"""armtc.py's rule on a PATH this test makes: a missing tool is a skip unless KVASIR_TEST_REQUIRE names its group,
then a failure; an unknown group name is an error; a present tool runs; first() and toolchains() follow the same rule.

    python3 tests/tooling/test_armtc.py -v
"""

import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))
import armtc  # noqa: E402


def outcome(body):
    """Runs body(case) in a case of its own; what came of it: ok, skip, fail or error, and the message."""

    # local: the loader would collect a module-level one as a test
    class Inner(unittest.TestCase):
        def runTest(self):
            body(self)

    result = unittest.TestResult()
    Inner().run(result)
    if result.errors:
        return 'error', result.errors[0][1]
    if result.failures:
        return 'fail', result.failures[0][1]
    if result.skipped:
        return 'skip', result.skipped[0][1]
    return 'ok', ''


class Armtc(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.bin = Path(self.tmp.name)
        # present; clang++, ld.lld and the rest missing
        for exe in ('arm-none-eabi-g++', 'llvm-objcopy'):
            p = self.bin / exe
            p.write_text('#!/bin/sh\n')
            p.chmod(0o755)
        self.env = mock.patch.dict(os.environ, {'PATH': str(self.bin)})
        self.env.start()
        os.environ.pop(armtc.ENV, None)

    def tearDown(self):
        self.env.stop()
        self.tmp.cleanup()

    def test_missing_unrequired_skips(self):
        kind, msg = outcome(lambda c: armtc.need(c, 'clang++'))
        self.assertEqual(kind, 'skip')
        self.assertIn('clang++', msg)

    def test_missing_required_fails(self):
        os.environ[armtc.ENV] = 'arm-gcc,clang'
        kind, msg = outcome(lambda c: armtc.need(c, 'clang++'))
        self.assertEqual(kind, 'fail')
        self.assertIn(
            'clang++ not found, but tool group clang is required', msg)

    def test_other_group_required_still_skips(self):
        os.environ[armtc.ENV] = 'arm-gcc'
        self.assertEqual(
            outcome(lambda c: armtc.need(c, 'clang++'))[0], 'skip')

    def test_all(self):
        os.environ[armtc.ENV] = 'all'
        self.assertEqual(
            outcome(lambda c: armtc.need(c, 'llvm-cov'))[0], 'fail')

    def test_unknown_group_is_an_error(self):
        os.environ[armtc.ENV] = 'clang,arm-gc'
        kind, msg = outcome(lambda c: armtc.need(c, 'clang++'))
        self.assertEqual(kind, 'error')
        self.assertIn('arm-gc', msg)

    def test_present_runs(self):
        os.environ[armtc.ENV] = 'all'
        ran = []
        self.assertEqual(outcome(lambda c: (armtc.need(
            c, 'arm-none-eabi-g++'), ran.append(1)))[0], 'ok')
        self.assertEqual(ran, [1])

    def test_ungrouped_never_required(self):
        os.environ[armtc.ENV] = 'all'
        self.assertEqual(outcome(lambda c: armtc.need(
            c, 'no-such-tool-anywhere'))[0], 'skip')

    def test_first(self):
        os.environ[armtc.ENV] = 'all'
        found = []
        self.assertEqual(outcome(lambda c: found.append(armtc.first(c, 'arm-none-eabi-objcopy', 'llvm-objcopy')))[0],
                         'ok')
        self.assertEqual(found, [str(self.bin / 'llvm-objcopy')])
        self.assertEqual(outcome(lambda c: armtc.first(
            c, 'llvm-nm', 'arm-none-eabi-nm'))[0], 'fail')
        os.environ.pop(armtc.ENV)
        self.assertEqual(outcome(lambda c: armtc.first(
            c, 'llvm-nm', 'arm-none-eabi-nm'))[0], 'skip')

    def test_toolchains(self):
        got = []
        self.assertEqual(
            outcome(lambda c: got.extend(armtc.toolchains(c)))[0], 'ok')
        self.assertEqual(got, ['gcc'])   # clang++ and ld.lld missing: left out
        os.environ[armtc.ENV] = 'arm-gcc'
        self.assertEqual(outcome(lambda c: armtc.toolchains(c))[0], 'ok')
        os.environ[armtc.ENV] = 'arm-gcc,clang'
        self.assertEqual(outcome(lambda c: armtc.toolchains(c))[0], 'fail')

    def test_toolchains_none_usable_skips(self):
        (self.bin / 'arm-none-eabi-g++').unlink()
        self.assertEqual(outcome(lambda c: armtc.toolchains(c))[0], 'skip')

    def test_no_case(self):
        with self.assertRaises(unittest.SkipTest):
            armtc.need(None, 'clang')
        os.environ[armtc.ENV] = 'clang'
        with self.assertRaises(AssertionError):
            armtc.need(None, 'clang')


if __name__ == '__main__':
    unittest.main()
