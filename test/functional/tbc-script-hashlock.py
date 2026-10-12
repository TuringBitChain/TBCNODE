#!/usr/bin/env python3
# Copyright (c) 2026 The TuringBitChain developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Lock and spend a real regtest UTXO using an existing-opcode SHA256 script.

The script requires the preimage b"hello". It does not require a signature:
anyone who knows that preimage can spend the output. Use only isolated regtest
funds. Transaction ids come from the node because the Python framework's
legacy rehash() does not implement version-10 TuringTXID.
"""

import hashlib
from decimal import Decimal
from io import BytesIO
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from contrib.script_examples.hashlock import (
    build_locking_script,
    build_unlocking_script,
)

from test_framework.mininode import CTransaction, CTxOut
from test_framework.script import CScript
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error


PREIMAGE = b"hello"
EXPECTED_HASH = bytes.fromhex(
    "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824")
ATOMIC_PER_TBC = 1_000_000
LOCKED_AMOUNT = Decimal("1.000000")
FEE = Decimal("0.001000")


def build_hashlock():
    """Hash the supplied stack item, push the expected digest, and compare."""
    return CScript(build_locking_script(EXPECTED_HASH))


def read_wire_transaction(raw_hex):
    """Read wire fields without calculating the legacy Python transaction id."""
    transaction = CTransaction()
    transaction.deserialize(BytesIO(bytes.fromhex(raw_hex)))
    return transaction


class HashlockTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1
        self.extra_args = [[
            "-txindex=1",
            "-genesisactivationheight=1",
            "-acceptnonstdtxn=1",
            "-acceptnonstdoutputs=1",
            "-listen=0",
            "-connect=0",
            "-dnsseed=0",
            "-discover=0",
        ]]

    def make_spend(self, funding_txid, address, unlocking_script):
        node = self.nodes[0]
        raw = node.createrawtransaction(
            [{"txid": funding_txid, "vout": 0}],
            {address: LOCKED_AMOUNT - FEE})
        transaction = read_wire_transaction(raw)
        assert_equal(transaction.nVersion, 10)
        transaction.vin[0].scriptSig = unlocking_script
        return transaction.serialize().hex()

    def assert_rejected(self, funding_txid, raw_hex, script_error):
        node = self.nodes[0]
        assert_raises_rpc_error(
            -26, script_error, node.sendrawtransaction, raw_hex)
        assert_equal(node.getrawmempool(), [])
        assert_equal(node.gettxout(funding_txid, 0)["value"], LOCKED_AMOUNT)
        self.log.info("Rejected (RPC -26): %s", script_error)

    def run_test(self):
        node = self.nodes[0]
        assert_equal(node.getblockchaininfo()["chain"], "regtest")

        self.log.info("1. Build the SHA256 hashlock and inspect its encoding")
        assert_equal(hashlib.sha256(PREIMAGE).digest(), EXPECTED_HASH)
        locking_script = build_hashlock()
        expected_script_hex = (
            "a8202cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e"
            "73043362938b982487")
        assert_equal(locking_script.hex(), expected_script_hex)
        assert_equal(len(locking_script), 35)
        assert_equal(node.decodescript(locking_script.hex())["asm"],
                     "OP_SHA256 " + EXPECTED_HASH.hex() + " OP_EQUAL")
        self.log.info("Locking script: %s", locking_script.hex())

        self.log.info("2. Mine regtest funds and create a 1 TBC hashlocked output")
        node.generate(2)
        coin = node.listunspent(2)[0]
        change_address = node.getnewaddress()
        raw_funding = node.createrawtransaction(
            [{"txid": coin["txid"], "vout": coin["vout"]}],
            {change_address: coin["amount"] - LOCKED_AMOUNT - FEE})
        funding = read_wire_transaction(raw_funding)
        assert_equal(funding.nVersion, 10)
        funding.vout.insert(
            0, CTxOut(int(LOCKED_AMOUNT * ATOMIC_PER_TBC), locking_script))
        signed = node.signrawtransaction(funding.serialize().hex())
        assert_equal(signed["complete"], True)
        funding_decoded = node.decoderawtransaction(signed["hex"])
        assert_equal(funding_decoded["vout"][0]["value"], LOCKED_AMOUNT)
        assert_equal(funding_decoded["vout"][0]["scriptPubKey"]["hex"],
                     expected_script_hex)
        funding_txid = node.sendrawtransaction(signed["hex"])
        assert_equal(funding_txid, funding_decoded["txid"])
        node.generate(1)
        assert_equal(node.gettxout(funding_txid, 0)["value"], LOCKED_AMOUNT)
        self.log.info("Funding TXID: %s (output 0)", funding_txid)

        receive_address = node.getnewaddress()
        self.log.info("3. Reject a wrong preimage without consuming the UTXO")
        wrong = self.make_spend(
            funding_txid, receive_address, CScript(build_unlocking_script(b"world")))
        self.assert_rejected(
            funding_txid, wrong,
            "Script evaluated without error but finished with a false/empty "
            "top stack element")

        self.log.info("4. Reject a missing preimage: SHA256 has no stack item")
        missing = self.make_spend(funding_txid, receive_address, CScript())
        self.assert_rejected(
            funding_txid, missing,
            "Operation not valid with the current stack size")

        self.log.info("5. Unlock with hello and confirm the spend in a block")
        correct = self.make_spend(
            funding_txid, receive_address, CScript(build_unlocking_script(PREIMAGE)))
        decoded = node.decoderawtransaction(correct)
        assert_equal(decoded["vin"][0]["scriptSig"]["hex"], "0568656c6c6f")
        spend_txid = node.sendrawtransaction(correct)
        assert_equal(spend_txid, decoded["txid"])
        assert_equal(node.getrawmempool(), [spend_txid])
        assert_equal(node.gettxout(funding_txid, 0, True), None)
        assert_equal(node.gettxout(funding_txid, 0, False)["value"],
                     LOCKED_AMOUNT)
        blockhash = node.generate(1)[0]
        block = node.getblock(blockhash, 1)
        assert spend_txid in block["tx"]
        assert_equal(node.getrawmempool(), [])
        assert_equal(node.gettxout(funding_txid, 0, False), None)
        received = node.gettxout(spend_txid, 0)
        assert_equal(received["value"], Decimal("0.999000"))
        assert_equal(received["confirmations"], 1)
        self.log.info("Spend TXID: %s", spend_txid)
        self.log.info("Confirmed block: %s", blockhash)
        self.log.info("Received 0.999000 TBC; fee 0.001000 TBC")


if __name__ == "__main__":
    HashlockTest().main()
