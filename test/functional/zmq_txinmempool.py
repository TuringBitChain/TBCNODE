#!/usr/bin/env python3
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test notifications from actual ordinary-mempool admission and removal."""

from decimal import Decimal
import json
import os
from pathlib import Path
import time

from test_framework.test_framework import BitcoinTestFramework, SkipTest
from test_framework.util import (assert_equal, assert_raises_rpc_error,
                                 check_zmq_test_requirements, disconnect_nodes_bi,
                                 wait_until, zmq_port)


class TxInMempoolTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2

    def add_options(self, parser):
        parser.add_option("--ipc", action="store_true", default=False,
                          help="Use an IPC publisher instead of TCP")

    def setup_nodes(self):
        check_zmq_test_requirements(self.options.configfile,
                                   SkipTest("bitcoind has not been built with zmq enabled"))
        if self.options.ipc and os.name == "nt":
            raise SkipTest("IPC transport is not available on Windows")
        self.address = ("ipc://" + str(Path(self.options.tmpdir) / "mempool.sock")
                        if self.options.ipc else "tcp://127.0.0.1:{}".format(zmq_port(0)))
        self.extra_args = [["-zmqpubtxinmempool=" + self.address,
                            "-zmqpubhashblock=" + self.address, "-mempoolexpiry=1"], []]
        super().setup_nodes()

    def signed_spend(self, coin, fee=Decimal("0.01")):
        node = self.nodes[0]
        raw = node.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                        {node.getnewaddress(): coin["amount"] - fee})
        signed = node.signrawtransaction(raw)
        assert_equal(signed["complete"], True)
        return raw, signed["hex"]

    def event(self, txid, reason=None):
        while True:
            frames = self.subscriber.recv_multipart()
            assert_equal(len(frames), 3)
            if frames[0] != b"hashblock":
                break
        topic, body, metadata = frames
        assert_equal(topic, b"txinmempool")
        assert_equal(len(body), 32)
        assert_equal(body.hex(), txid)
        self.sequence += 1
        expected = {"epoch": self.epoch, "seq": self.sequence,
                    "status": "ACCEPTED" if reason is None else "DISCARDED"}
        if reason is not None:
            expected["reason"] = reason
        assert_equal(json.loads(metadata), expected)
        # The underlying pool mutation must be visible by the time of delivery.
        assert_equal(txid in self.nodes[0].getrawmempool(), reason is None)

    def no_event(self):
        while self.subscriber.poll(500):
            assert_equal(self.subscriber.recv_multipart()[0], b"hashblock")

    def run_test(self):
        import zmq
        context = zmq.Context()
        self.subscriber = context.socket(zmq.SUB)
        self.subscriber.setsockopt(zmq.RCVTIMEO, 10000)
        self.subscriber.setsockopt(zmq.SUBSCRIBE, b"txinmempool")
        self.subscriber.setsockopt(zmq.SUBSCRIBE, b"hashblock")
        self.subscriber.connect(self.address)
        try:
            self.check_events()
        finally:
            self.subscriber.close(linger=0)
            context.term()

    def check_events(self):
        node, miner = self.nodes
        self.epoch = int((Path(node.datadir) / "regtest" / "txinmempool.epoch").read_text())
        self.sequence = 0
        # Establish subscription readiness without generating any mempool event.
        for _ in range(30):
            node.generate(1)
            if self.subscriber.poll(200):
                assert_equal(self.subscriber.recv_multipart()[0], b"hashblock")
                break
        else:
            raise AssertionError("ZMQ subscription did not become ready")
        self.sync_all()
        disconnect_nodes_bi(self.nodes, 0, 1)
        coins = node.listunspent(100)
        assert len(coins) >= 5

        self.log.info("Check admission, duplicate submission, mining and coinbase exclusion")
        _, signed = self.signed_spend(coins.pop())
        txid = node.sendrawtransaction(signed)
        self.event(txid)
        assert_raises_rpc_error(-27, "Transaction already in the mempool", node.sendrawtransaction, signed)
        self.no_event()
        block = node.generate(1)[0]
        self.event(txid, "included-in-block")
        self.no_event()
        assert_equal(miner.submitblock(node.getblock(block, False)), None)

        self.log.info("Check script, fee and double-spend rejections produce no events")
        coin = coins.pop()
        unsigned, signed = self.signed_spend(coin)
        assert_raises_rpc_error(-26, "", node.sendrawtransaction, unsigned)
        _, free = self.signed_spend(coin, Decimal("0"))
        assert_raises_rpc_error(-26, "", node.sendrawtransaction, free)
        self.no_event()
        txid = node.sendrawtransaction(signed)
        self.event(txid)
        _, conflicting = self.signed_spend(coin, Decimal("0.02"))
        assert_raises_rpc_error(-26, "", node.sendrawtransaction, conflicting)
        self.no_event()

        self.log.info("Check a block-only conflicting transaction removes the local entry")
        competing_id = miner.sendrawtransaction(conflicting)
        block = miner.generate(1)[0]
        assert competing_id in miner.getblock(block)["tx"]
        assert_equal(node.submitblock(miner.getblock(block, False)), None)
        self.event(txid, "collision-in-block-tx")
        self.no_event()

        self.log.info("Check parent admission order and expiry of the parent and descendant")
        _, signed = self.signed_spend(coins.pop())
        parent = node.sendrawtransaction(signed)
        self.event(parent)
        decoded = node.decoderawtransaction(signed)
        _, child_raw = self.signed_spend({"txid": parent, "vout": 0,
                                          "amount": decoded["vout"][0]["value"]})
        child = node.sendrawtransaction(child_raw)
        self.event(child)
        node.setmocktime(int(time.time()) + 7200)
        _, signed = self.signed_spend(coins.pop())
        trigger = node.sendrawtransaction(signed)
        self.event(trigger)
        # Descendant removal order is intentionally unspecified.
        expired = set()
        for _ in range(2):
            frames = self.subscriber.recv_multipart()
            assert_equal(len(frames), 3)
            assert_equal(frames[0], b"txinmempool")
            self.sequence += 1
            assert_equal(json.loads(frames[2]), {"epoch": self.epoch, "seq": self.sequence,
                                               "status": "DISCARDED", "reason": "expired"})
            expired.add(frames[1].hex())
        assert_equal(expired, {parent, child})
        wait_until(lambda: set(node.getrawmempool()) == {trigger})
        self.no_event()


if __name__ == '__main__':
    TxInMempoolTest().main()
