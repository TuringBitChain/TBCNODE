#!/usr/bin/env python3
# Copyright (c) 2026 The TuringBitChain developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test the hashlock builder and its real command-line interface."""

import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from contrib.script_examples.hashlock import (
    build_locking_script,
    build_unlocking_script,
    inspect_hashlock,
)


HELLO_HASH = bytes.fromhex(
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824")
HELLO_LOCK_HEX = (
    "a8202cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e"
    "73043362938b982487")
ENTRY_POINT = Path(__file__).with_name("hashlock.py")


class HashlockBuilderTest(unittest.TestCase):
    def test_lock_has_sha256_digest_push_and_equal(self):
        self.assertEqual(build_locking_script(HELLO_HASH).hex(),
                         HELLO_LOCK_HEX)

    def test_lock_rejects_non_sha256_digest_sizes(self):
        for size in (0, 31, 33):
            with self.subTest(size=size), self.assertRaises(ValueError):
                build_locking_script(bytes(size))

    def test_unlock_pushes_hello_bytes(self):
        self.assertEqual(build_unlocking_script(b"hello").hex(),
                         "0568656c6c6f")

    def test_empty_preimage_is_a_stack_item_not_a_missing_script(self):
        self.assertEqual(build_unlocking_script(b"").hex(), "00")
        self.assertNotEqual(build_unlocking_script(b""), b"")

    def test_single_byte_preimages_use_exact_minimal_pushes(self):
        fixtures = (
            (b"\x01", "51"),
            (b"\x10", "60"),
            (b"\x81", "4f"),
            (b"\x00", "0100"),
        )
        for preimage, script_hex in fixtures:
            with self.subTest(preimage=preimage):
                self.assertEqual(build_unlocking_script(preimage).hex(),
                                 script_hex)

    def test_demo_supports_direct_push_up_to_75_bytes(self):
        self.assertEqual(build_unlocking_script(b"a" * 75),
                         b"\x4b" + b"a" * 75)
        with self.assertRaises(ValueError):
            build_unlocking_script(b"a" * 76)

    def test_inspect_reports_hello_hash_and_script_sizes(self):
        result = inspect_hashlock(b"hello")
        self.assertIn("preimage_sha256", result)
        self.assertEqual(result["preimage_sha256"], HELLO_HASH.hex())
        self.assertEqual(result["scriptPubKey"]["hex"], HELLO_LOCK_HEX)
        self.assertEqual(result["scriptPubKey"]["size"], 35)
        self.assertEqual(result["scriptSig"]["hex"], "0568656c6c6f")
        self.assertEqual(result["scriptSig"]["size"], 6)
        self.assertTrue(result["hash_matches"])
        self.assertTrue(result["offline_only"])

    def test_wrong_preimage_does_not_replace_the_expected_hash(self):
        result = inspect_hashlock(b"world", HELLO_HASH)
        self.assertIn("hash_matches", result)
        self.assertFalse(result["hash_matches"])
        self.assertEqual(result["scriptPubKey"]["hex"], HELLO_LOCK_HEX)
        self.assertEqual(result["scriptSig"]["hex"], "05776f726c64")


class HashlockCommandTest(unittest.TestCase):
    def run_command(self, *arguments, cwd=None):
        return subprocess.run(
            [sys.executable, str(ENTRY_POINT), *arguments],
            cwd=cwd, text=True, capture_output=True, check=False)

    def test_inspect_runs_from_outside_the_repository(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_command(
                "inspect", "--preimage", "hello", cwd=directory)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout)["scriptPubKey"]["hex"],
                         HELLO_LOCK_HEX)

    def test_inspect_encodes_text_as_utf8_bytes(self):
        result = self.run_command("inspect", "--preimage", "\u5b66\u4e60")
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertEqual(data["preimage_hex"], "e5ada6e4b9a0")
        self.assertEqual(data["scriptSig"]["hex"], "06e5ada6e4b9a0")

    def test_inspect_accepts_a_fixed_expected_hash(self):
        result = self.run_command(
            "inspect", "--preimage", "world",
            "--expected-hash", HELLO_HASH.hex())
        self.assertEqual(result.returncode, 0, result.stderr)
        data = json.loads(result.stdout)
        self.assertFalse(data["hash_matches"])
        self.assertEqual(data["scriptPubKey"]["hex"], HELLO_LOCK_HEX)

    def test_inspect_rejects_invalid_hash_without_a_traceback(self):
        result = self.run_command(
            "inspect", "--preimage", "hello", "--expected-hash", "xyz")
        self.assertEqual(result.returncode, 2)
        self.assertIn("64 hexadecimal characters", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_inspect_rejects_oversized_demo_preimage(self):
        result = self.run_command("inspect", "--preimage", "a" * 76)
        self.assertEqual(result.returncode, 2)
        self.assertIn("75 bytes", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_demo_refuses_to_reuse_an_existing_directory(self):
        with tempfile.TemporaryDirectory() as directory:
            result = self.run_command("demo", "--tmpdir", directory)
            self.assertEqual(result.returncode, 2)
            self.assertIn("already exists", result.stderr)
            self.assertTrue(Path(directory).is_dir())


if __name__ == "__main__":
    unittest.main()
