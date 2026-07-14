#!/usr/bin/env python3
# Copyright (c) 2026 TBC developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test wallet chain-limit selection ignores deprecated descendant limit."""

from decimal import Decimal

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, wait_until


class WalletDescendantLimitIgnoredTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [[
            "-walletrejectlongchains",
            "-limitancestorcount=10",
            "-limitdescendantcount=1",
        ]]

    def run_test(self):
        node = self.nodes[0]
        node.generate(101)

        # Consolidate mature balance into one confirmed wallet output so
        # subsequent sends have to spend unconfirmed change.
        self_addr = node.getnewaddress()
        consolidate_txid = node.sendtoaddress(
            self_addr, node.getbalance(), "", "", True)
        node.generate(1)
        assert(consolidate_txid not in node.getrawmempool())

        dest = node.getnewaddress()
        txids = []
        for _ in range(3):
            txid = node.sendtoaddress(dest, Decimal("0.0001"))
            txids.append(txid)
            wait_until(lambda: txid in node.getrawmempool(), timeout=5)

        assert_equal(len(set(txids).intersection(set(node.getrawmempool()))), 3)


if __name__ == '__main__':
    WalletDescendantLimitIgnoredTest().main()
