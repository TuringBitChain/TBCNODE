#!/usr/bin/env python3
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test txinmempool across orphan promotion, finalisation, reorg and restart."""

import copy
import json
import os
import time
from decimal import Decimal
from pathlib import Path

from test_framework.blocktools import create_block, create_coinbase
from test_framework.mininode import (CTransaction, CTxIn, CTxOut, COutPoint,
                                     FromHex, ToHex, NodeConn, NodeConnCB,
                                     NetworkThread, msg_tx)
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (assert_equal, assert_raises_rpc_error, check_zmq_test_requirements,
                                 p2p_port, wait_until, zmq_port)


class TxInMempoolLifecycleTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def add_options(self, parser):
        parser.add_option("--ipc", action="store_true", default=False)

    def setup_nodes(self):
        check_zmq_test_requirements(self.options.configfile,
                                   SkipTest("bitcoind has not been built with zmq enabled"))
        if self.options.ipc and os.name == "nt":
            raise SkipTest("IPC transport is not available on Windows")
        self.address = ("ipc://" + str(Path(self.options.tmpdir) / "lifecycle.sock")
                        if self.options.ipc else "tcp://127.0.0.1:{}".format(zmq_port(0)))
        self.base_args = ["-genesisactivationheight=100", "-checknonfinalfreq=100",
                          "-txnvalidationasynchrunfreq=1", "-persistmempool=1"]
        self.enabled_args = self.base_args + ["-zmqpubtxinmempool=" + self.address,
                                              "-zmqpubhashblock=" + self.address]
        self.extra_args = [self.enabled_args]
        super().setup_nodes()

    def subscribe(self):
        import zmq
        self.subscriber = self.context.socket(zmq.SUB)
        self.subscriber.setsockopt(zmq.RCVTIMEO, 10000)
        self.subscriber.setsockopt(zmq.SUBSCRIBE, b"txinmempool")
        self.subscriber.setsockopt(zmq.SUBSCRIBE, b"hashblock")
        self.subscriber.connect(self.address)

    def reset_epoch(self):
        self.epoch = int(self.epoch_file.read_text())
        self.sequence = 0
        assert_equal(self.node.getzmqtipsnapshot(), {"epoch": self.epoch, "seq": 0})

    def empty_block(self, previous):
        tip = self.node.getblock(previous)
        # Cached chains can be older than the IBD tip-age limit. Use a fresh
        # timestamp so hashblock can acknowledge subscription readiness.
        block = create_block(int(previous, 16), create_coinbase(tip["height"] + 1),
                             max(tip["time"] + 1, int(time.time())))
        block.solve()
        # An equal-work side block is stored before it becomes the active tip.
        result = self.node.submitblock(ToHex(block))
        assert result in (None, "inconclusive"), result
        assert_equal(self.node.getblockheader(block.hash)["hash"], block.hash)
        return block.hash

    def warm_up(self):
        # Empty blocks acknowledge the subscription without mining restored
        # entries or allocating a txinmempool event position.
        for _ in range(30):
            self.empty_block(self.node.getbestblockhash())
            if self.subscriber.poll(200):
                assert_equal(self.subscriber.recv_multipart()[0], b"hashblock")
                return
        raise AssertionError("ZMQ subscription did not become ready")

    def no_event(self):
        while self.subscriber.poll(300):
            assert_equal(self.subscriber.recv_multipart()[0], b"hashblock")

    def event(self, expected_ids, reason=None):
        while True:
            frames = self.subscriber.recv_multipart()
            assert_equal(len(frames), 3)
            if frames[0] != b"hashblock":
                break
        assert_equal(frames[0], b"txinmempool")
        assert_equal(len(frames[1]), 32)
        txid = frames[1].hex()
        assert txid in expected_ids, (txid, expected_ids)
        self.sequence += 1
        expected = {"epoch": self.epoch, "seq": self.sequence,
                    "status": "ACCEPTED" if reason is None else "DISCARDED"}
        if reason is not None:
            expected["reason"] = reason
        assert_equal(json.loads(frames[2]), expected)
        wait_until(lambda: self.node.getzmqtipsnapshot()["seq"] >= self.sequence)
        assert_equal(self.node.getzmqtipsnapshot()["epoch"], self.epoch)
        if reason is None:
            entry = self.node.getmempoolentry(txid)
            assert_equal((entry["epoch"], entry["seq"]), (self.epoch, self.sequence))
        else:
            assert_raises_rpc_error(-5, "Transaction not in mempool", self.node.getmempoolentry, txid)
        return txid

    def accepted(self, *transactions):
        for tx in transactions:
            self.event({tx.hash})

    def discarded(self, transactions, reason="included-in-block"):
        remaining = {tx.hash for tx in transactions}
        while remaining:
            remaining.remove(self.event(remaining, reason))

    def spend(self, parent, output=0, locktime=0, sequence=0xffffffff):
        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(int(parent.hash, 16), output), b'', sequence))
        tx.vout.append(CTxOut(parent.vout[output].nValue - 1000, CScript([OP_TRUE])))
        tx.nLockTime = locktime
        tx.rehash()
        return tx

    def submit(self, tx):
        assert_equal(self.node.sendrawtransaction(ToHex(tx)), tx.hash)

    def mine(self, transactions):
        block = self.node.generate(1)[0]
        assert {tx.hash for tx in transactions}.issubset(self.node.getblock(block)["tx"])
        self.discarded(transactions)
        self.no_event()
        return block

    def check_orphans(self, funding):
        self.log.info("Orphan descendants stay silent until their parent is accepted")
        parent = self.spend(funding, 0)
        child = self.spend(parent)
        grandchild = self.spend(child)
        callback = NodeConnCB()
        connection = NodeConn('127.0.0.1', p2p_port(0), self.node, callback)
        callback.add_connection(connection)
        network = NetworkThread()
        network.start()
        try:
            callback.wait_for_verack()
            logfile = Path(self.node.datadir) / "regtest" / "bitcoind.log"
            for tx in (grandchild, child):
                connection.send_message(msg_tx(tx))
                wait_until(lambda: "stored orphan txn= " + tx.hash in logfile.read_text(), timeout=10)
                assert_equal(self.node.getrawmempool(), [])
                self.no_event()
            connection.send_message(msg_tx(parent))
            wait_until(lambda: set(self.node.getrawmempool()) == {parent.hash, child.hash, grandchild.hash})
            self.accepted(parent, child, grandchild)
            self.mine([parent, child, grandchild])
        finally:
            connection.close()
            network.join(timeout=10)
            assert not network.is_alive()

    def check_nonfinal(self, funding):
        self.log.info("Non-final updates stay silent; explicit finalisation emits once")
        pending = self.spend(funding, 1, self.node.getblockcount() + 100, 1)
        self.submit(pending)
        wait_until(lambda: self.node.getrawnonfinalmempool() == [pending.hash])
        assert_equal(self.node.getrawmempool(), [])
        self.no_event()
        updated = copy.deepcopy(pending)
        updated.vin[0].nSequence = 2
        updated.rehash()
        self.submit(updated)
        wait_until(lambda: self.node.getrawnonfinalmempool() == [updated.hash])
        self.no_event()
        final = copy.deepcopy(updated)
        final.vin[0].nSequence = 0xffffffff
        final.rehash()
        self.submit(final)
        wait_until(lambda: self.node.getrawmempool() == [final.hash])
        assert_equal(self.node.getrawnonfinalmempool(), [])
        self.accepted(final)
        self.mine([final])

        self.log.info("Height unlocking emits ACCEPTED for the original non-final txid")
        unlocked = self.spend(funding, 2, self.node.getblockcount() + 1, 1)
        self.submit(unlocked)
        wait_until(lambda: self.node.getrawnonfinalmempool() == [unlocked.hash])
        self.no_event()
        self.node.generate(1)
        wait_until(lambda: self.node.getrawmempool() == [unlocked.hash])
        assert_equal(self.node.getrawnonfinalmempool(), [])
        self.accepted(unlocked)
        self.mine([unlocked])

    def wallet_spend(self, txid, value):
        raw = self.node.createrawtransaction([{"txid": txid, "vout": 0}],
                                            {self.node.getnewaddress(): value - Decimal("0.01")})
        signed = self.node.signrawtransaction(raw)["hex"]
        result = self.node.sendrawtransaction(signed)
        self.event({result})
        return result

    def check_reorg(self, funding):
        self.log.info("A longer competing chain returns parent and child in dependency order")
        parent = self.spend(funding, 3)
        child = self.spend(parent)
        self.submit(parent)
        self.submit(child)
        self.accepted(parent, child)
        block = self.mine([parent, child])
        before = self.sequence
        fork_base = self.node.getblock(block)["previousblockhash"]
        fork_tip = self.empty_block(self.empty_block(fork_base))
        wait_until(lambda: self.node.getbestblockhash() == fork_tip)
        wait_until(lambda: set(self.node.getrawmempool()) == {parent.hash, child.hash})
        self.accepted(parent, child)
        assert_equal(self.sequence, before + 2)
        self.mine([parent, child])

        self.log.info("Reorg removes a spend and descendant when their coinbase is disconnected")
        # TBC coinbase maturity is one block, so spend the current tip's
        # coinbase and then remove the block that created it.
        blocks = self.node.generate(1)
        coinbase = self.node.getblock(blocks[0])["tx"][0]
        value = self.node.gettxout(coinbase, 0)["value"]
        parent_id = self.wallet_spend(coinbase, value)
        child_id = self.wallet_spend(parent_id, value - Decimal("0.01"))
        self.node.invalidateblock(blocks[-1])
        wait_until(lambda: self.node.getrawmempool() == [])
        remaining = {parent_id, child_id}
        while remaining:
            remaining.remove(self.event(remaining, "reorg"))
        self.no_event()

        self.log.info("Failed re-admission stays silent and removes an in-pool descendant")
        coinbase_block = self.node.generate(1)[0]
        coinbase = self.node.getblock(coinbase_block)["tx"][0]
        value = self.node.gettxout(coinbase, 0)["value"]
        parent_id = self.wallet_spend(coinbase, value)
        mined = self.node.generate(1)[0]
        assert parent_id in self.node.getblock(mined)["tx"]
        self.event({parent_id}, "included-in-block")
        child_id = self.wallet_spend(parent_id, value - Decimal("0.01"))
        self.node.invalidateblock(coinbase_block)
        wait_until(lambda: self.node.getrawmempool() == [])
        self.event({child_id}, "reorg")
        self.no_event()

    def restart(self, enabled, ordinary, nonfinal=()):
        previous_epoch = self.epoch
        self.stop_node(0)
        self.subscriber.close(linger=0)
        self.subscribe()
        self.start_node(0, self.enabled_args if enabled else self.base_args)
        wait_until(lambda: set(self.node.getrawmempool()) == {tx.hash for tx in ordinary})
        wait_until(lambda: set(self.node.getrawnonfinalmempool()) == {tx.hash for tx in nonfinal})
        for tx in ordinary:
            entry = self.node.getmempoolentry(tx.hash)
            assert "epoch" not in entry
            assert "seq" not in entry
        if enabled:
            self.reset_epoch()
            assert self.epoch > previous_epoch
            self.warm_up()
        else:
            assert_equal(int(self.epoch_file.read_text()), previous_epoch)
            assert_equal(self.node.getzmqtipsnapshot(), {"epoch": -1, "seq": -1})
        self.no_event()

    def check_persistence(self, funding):
        self.log.info("Restart restores ordinary and non-final entries without consuming event positions")
        parent = self.spend(funding, 4)
        child = self.spend(parent)
        pending = self.spend(funding, 5, self.node.getblockcount() + 1000, 1)
        self.submit(parent)
        self.submit(child)
        self.accepted(parent, child)
        self.submit(pending)
        wait_until(lambda: self.node.getrawnonfinalmempool() == [pending.hash])
        self.no_event()
        self.restart(True, [parent, child], [pending])
        block = self.mine([parent, child])
        assert_equal(self.sequence, 2)  # First events are the restored entries' removals.
        self.node.invalidateblock(block)
        wait_until(lambda: set(self.node.getrawmempool()) == {parent.hash, child.hash})
        self.accepted(parent, child)
        self.mine([parent, child])
        final = copy.deepcopy(pending)
        final.vin[0].nSequence = 0xffffffff
        final.rehash()
        self.submit(final)
        wait_until(lambda: self.node.getrawmempool() == [final.hash])
        self.accepted(final)
        self.mine([final])
        assert_equal(self.sequence, 8)

        self.log.info("Disabled startup preserves epoch; re-enabling silently restores its transactions")
        self.restart(False, [])
        disabled = self.spend(funding, 6)
        self.submit(disabled)
        assert_equal(self.node.getrawmempool(), [disabled.hash])
        self.no_event()
        self.restart(True, [disabled])
        self.mine([disabled])
        assert_equal(self.sequence, 1)

    def run_test(self):
        import zmq
        self.node = self.nodes[0]
        self.context = zmq.Context()
        self.subscribe()
        self.epoch_file = Path(self.node.datadir) / "regtest" / "txinmempool.epoch"
        self.reset_epoch()
        try:
            self.warm_up()
            funding = CTransaction()
            funding.vout = [CTxOut(100000, CScript([OP_TRUE])) for _ in range(7)]
            raw = self.node.fundrawtransaction(ToHex(funding), {"changePosition": 7})["hex"]
            signed = self.node.signrawtransaction(raw)["hex"]
            funding = FromHex(CTransaction(), signed)
            funding.rehash()
            self.submit(funding)
            self.accepted(funding)
            self.mine([funding])
            self.check_orphans(funding)
            self.check_nonfinal(funding)
            self.check_reorg(funding)
            self.check_persistence(funding)
        finally:
            self.subscriber.close(linger=0)
            self.context.term()


if __name__ == '__main__':
    TxInMempoolLifecycleTest().main()
