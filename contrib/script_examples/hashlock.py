#!/usr/bin/env python3
# Copyright (c) 2026 The TuringBitChain developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Standalone entry point for the existing-opcode SHA256 hashlock exercise."""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import subprocess
import sys


def build_locking_script(expected_hash):
    """Encode OP_SHA256 <32-byte digest> OP_EQUAL, without executing it."""
    if len(expected_hash) != 32:
        raise ValueError("Expected a 32-byte SHA256 digest")
    return b"\xa8\x20" + expected_hash + b"\x87"


def build_unlocking_script(preimage):
    """Encode one minimal data push; this learning tool allows up to 75 bytes."""
    if len(preimage) > 75:
        raise ValueError("This learning example supports at most 75 bytes")
    if not preimage:
        return b"\x00"
    if len(preimage) == 1 and 1 <= preimage[0] <= 16:
        return bytes([0x50 + preimage[0]])
    if preimage == b"\x81":
        return b"\x4f"
    return bytes([len(preimage)]) + preimage


def inspect_hashlock(preimage, expected_hash=None):
    """Report encoding and a hash comparison, not full transaction validation."""
    unlocking_script = build_unlocking_script(preimage)
    digest = hashlib.sha256(preimage).digest()
    if expected_hash is None:
        expected_hash = digest
    locking_script = build_locking_script(expected_hash)
    return {
        "offline_only": True,
        "preimage_hex": preimage.hex(),
        "preimage_sha256": digest.hex(),
        "expected_hash": expected_hash.hex(),
        "hash_matches": digest == expected_hash,
        "scriptPubKey": {
            "asm": "OP_SHA256 " + expected_hash.hex() + " OP_EQUAL",
            "hex": locking_script.hex(),
            "size": len(locking_script),
        },
        "scriptSig": {
            "hex": unlocking_script.hex(),
            "size": len(unlocking_script),
        },
        "warning": (
            "Regtest learning only. There is no signature requirement: "
            "anyone who learns the preimage can spend the output. "
            "Do not use real secrets in shell arguments."),
    }


def parse_expected_hash(text):
    """Require unambiguous, fixed-width hex rather than accepting whitespace."""
    if re.fullmatch(r"[0-9a-fA-F]{64}", text) is None:
        raise argparse.ArgumentTypeError(
            "Expected exactly 64 hexadecimal characters (32 bytes)")
    return bytes.fromhex(text)


def run_demo(arguments, parser):
    """Run the existing real-node test in a new, isolated regtest directory."""
    if arguments.tmpdir is not None and arguments.tmpdir.exists():
        parser.error("Test directory already exists; choose a new directory")
    if importlib.util.find_spec("asyncore") is None:
        parser.error(
            "The node test framework requires asyncore; use Python 3.9-3.11 "
            "(on this Mac: /usr/bin/python3)")

    repository = Path(__file__).resolve().parents[2]
    binaries = repository / "build" / "src"
    config = repository / "build" / "test" / "config.ini"
    functional_test = repository / "test" / "functional" / "tbc-script-hashlock.py"
    for required in (binaries / "bitcoind", config, functional_test):
        if not required.is_file():
            parser.error("Required file not found: " + str(required))

    command = [
        sys.executable, str(functional_test),
        "--srcdir=" + str(binaries),
        "--configfile=" + str(config),
    ]
    if arguments.tmpdir is not None:
        command.append("--tmpdir=" + str(arguments.tmpdir.resolve()))
    if arguments.keep_data:
        command.append("--nocleanup")

    print(
        "Running a new regtest node; existing nodes and wallets are not used.",
        file=sys.stderr, flush=True)
    return subprocess.run(command, cwd=repository, check=False).returncode


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Build and exercise a SHA256 hashlock using existing TBC opcodes.")
    commands = parser.add_subparsers(dest="command", required=True)
    inspect = commands.add_parser(
        "inspect", help="Print script bytes offline; no node or funds are used")
    inspect.add_argument(
        "--preimage", required=True, help="Public demo text, encoded as UTF-8")
    inspect.add_argument(
        "--expected-hash", type=parse_expected_hash,
        help="Keep this 32-byte hash fixed when inspecting a wrong preimage")
    demo = commands.add_parser(
        "demo", help="Run funding, failed unlocks, successful spend and confirmation")
    demo.add_argument("--tmpdir", type=Path, help="A new, non-existing test directory")
    demo.add_argument(
        "--keep-data", action="store_true",
        help="Keep test logs and regtest data after the node stops")
    arguments = parser.parse_args(argv)

    if arguments.command == "demo":
        return run_demo(arguments, parser)
    try:
        result = inspect_hashlock(
            arguments.preimage.encode("utf-8"), arguments.expected_hash)
    except ValueError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
