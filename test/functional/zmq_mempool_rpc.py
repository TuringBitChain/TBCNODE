#!/usr/bin/env python3
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test mempool notification RPCs, including operation without any subscriber."""

import configparser
from decimal import Decimal
from pathlib import Path

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_raises_rpc_error, wait_until, zmq_port


class MempoolNotificationRPCTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1

    def omitted(self, txid):
        entry = self.nodes[0].getmempoolentry(txid)
        assert "epoch" not in entry
        assert "seq" not in entry
        return entry

    def spend(self, coin):
        node = self.nodes[0]
        value = coin["amount"] - Decimal("0.01")
        raw = node.createrawtransaction([{"txid": coin["txid"], "vout": coin["vout"]}],
                                        {node.getnewaddress(): value})
        signed = node.signrawtransaction(raw)
        assert_equal(signed["complete"], True)
        txid = node.sendrawtransaction(signed["hex"])
        return {"txid": txid, "vout": 0, "amount": value}

    def snapshot(self, epoch, seq):
        expected = {"epoch": epoch, "seq": seq}
        wait_until(lambda: self.nodes[0].getzmqtipsnapshot() == expected)
        result = self.nodes[0].getzmqtipsnapshot()
        assert_equal(result, expected)
        assert all(type(value) is int for value in result.values())

    def run_test(self):
        node = self.nodes[0]
        self.log.info("Disabled notification RPCs are available even without ZMQ support")
        self.snapshot(-1, -1)
        assert "getzmqtipsnapshot" in node.help()
        assert "successfully submitted" in node.help("getzmqtipsnapshot")
        assert "epoch" in node.help("getmempoolentry")
        assert "seq" in node.help("getmempoolentry")
        assert_raises_rpc_error(-1, "getzmqtipsnapshot", node.getzmqtipsnapshot, 1)
        assert_raises_rpc_error(-5, "Transaction not in mempool", node.getmempoolentry, "ff" * 32)
        node.generate(1)
        coins = node.listunspent()
        original = self.spend(coins.pop())["txid"]
        baseline = self.omitted(original)
        assert_equal(node.getrawmempool(True)[original], baseline)
        self.snapshot(-1, -1)

        config = configparser.ConfigParser()
        config.read(self.options.configfile or Path(self.options.srcdir).parent / "test" / "config.ini")
        if not config["components"].getboolean("ENABLE_ZMQ"):
            self.log.info("Disabled-mode assertions passed; enabled-mode checks require ZMQ")
            return

        address = "tcp://127.0.0.1:{}".format(zmq_port(0))
        args = ["-zmqpubtxinmempool=" + address]
        epoch_file = Path(node.datadir) / "regtest" / "txinmempool.epoch"
        self.stop_node(0)
        self.start_node(0, args)
        wait_until(lambda: node.getrawmempool() == [original])
        epoch = int(epoch_file.read_text())
        self.snapshot(epoch, 0)
        assert_equal(self.omitted(original), baseline)

        self.log.info("Without subscribers, admission and successful submission positions still advance")
        first = self.spend(coins.pop())
        self.snapshot(epoch, 1)
        entry = node.getmempoolentry(first["txid"])
        assert_equal(set(entry), set(baseline) | {"epoch", "seq"})
        assert_equal((entry["epoch"], entry["seq"]), (epoch, 1))
        second = self.spend(first)
        self.snapshot(epoch, 2)
        entry = node.getmempoolentry(second["txid"])
        assert_equal((entry["epoch"], entry["seq"]), (epoch, 2))
        assert_equal(node.getmempoolentry(first["txid"])["seq"], 1)
        self.omitted(original)
        # Other verbose mempool RPC schemas retain their existing fields.
        verbose = node.getrawmempool(True)
        for entry in verbose.values():
            assert_equal(set(entry), set(baseline))
        assert "epoch" not in node.getmempoolancestors(second["txid"], True)[first["txid"]]
        assert "seq" not in node.getmempooldescendants(first["txid"], True)[second["txid"]]

        self.log.info("Removal and re-admission give new positions, while missing entries retain their error")
        block = node.generate(1)[0]
        self.snapshot(epoch, 5)
        txids = {original, first["txid"], second["txid"]}
        for txid in txids:
            assert_raises_rpc_error(-5, "Transaction not in mempool", node.getmempoolentry, txid)
        node.invalidateblock(block)
        wait_until(lambda: set(node.getrawmempool()) == txids)
        self.snapshot(epoch, 8)
        entries = {txid: node.getmempoolentry(txid) for txid in txids}
        assert_equal({entry["seq"] for entry in entries.values()}, {6, 7, 8})
        assert all(entry["epoch"] == epoch for entry in entries.values())
        assert entries[first["txid"]]["seq"] < entries[second["txid"]]["seq"]

        self.log.info("Restart renews epoch and omits acceptance fields on restored transactions")
        self.stop_node(0)
        self.start_node(0, args)
        wait_until(lambda: set(node.getrawmempool()) == txids)
        renewed = int(epoch_file.read_text())
        assert renewed > epoch
        self.snapshot(renewed, 0)
        for txid in txids:
            self.omitted(txid)

        self.log.info("Legacy-only ZMQ configuration still reports disabled txinmempool positions")
        self.stop_node(0)
        self.start_node(0, ["-zmqpubhashblock=" + address])
        wait_until(lambda: set(node.getrawmempool()) == txids)
        self.snapshot(-1, -1)
        for txid in txids:
            self.omitted(txid)
        assert_equal(int(epoch_file.read_text()), renewed)


if __name__ == '__main__':
    MempoolNotificationRPCTest().main()
