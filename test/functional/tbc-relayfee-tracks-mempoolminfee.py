#!/usr/bin/env python3
# Copyright (c) 2020 Bitcoin Association
# Distributed under the Open TBC software license, see the accompanying file LICENSE.

"""
Verify that the relay fee reported by the RPCs is unified with the mempool
minimum fee, and that the removed -minrelaytxfee knob no longer has any effect.

After dropping the static minrelaytxfee path, both mempool admission and the
advertised relay fee are sourced from the dynamic mempool minimum fee
(CTxMemPool::GetMinFee). This test asserts:

 1. getinfo.relayfee == getnetworkinfo.relayfee == getmempoolinfo.mempoolminfee
    on every node (the three are a single value).
 2. That value equals the configured -mempoolminfeerate (the flat-floor feerate
    while the mempool is empty), so two nodes configured with different
    mempoolminfeerate report different relay fees.
 3. A large, distinct -minrelaytxfee passed to both nodes is ignored: the relay
    fee never takes that value.

Note: this chain uses TBCCOIN (1e6) base units per coin. The dynamic ramp above
-mempoolfeerampstart is not exercised here because -maxmempool has a ~90MB lower
bound, so moving the fee through real usage would need millions of bytes of
mempool; the equality invariant above already pins relayfee to mempoolminfee.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal
from decimal import Decimal

# Base monetary units per coin on this chain (src/amount.h: TBCCOIN).
TBCCOIN = 1000000

# A deliberately large, distinct value for the (now ignored) -minrelaytxfee, so
# it cannot be confused with either node's mempool floor.
IGNORED_MINRELAYTXFEE = Decimal(1000) / TBCCOIN  # 0.001

class RelayFeeTracksMempoolMinFeeTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # Each node gets a different mempool floor so we can prove the relay fee
        # follows mempoolminfeerate (and not the shared, ignored minrelaytxfee).
        self.floor_sats = [50, 120]
        self.extra_args = [[
            "-mempoolminfeerate={}".format(rate),
            "-minrelaytxfee={}".format(IGNORED_MINRELAYTXFEE),
        ] for rate in self.floor_sats]

    def relay_fees(self, node):
        # The three values are computed independently from the mempool's current
        # GetMinFee(); with an idle mempool they must be identical.
        return (
            node.getmempoolinfo()['mempoolminfee'],
            node.getinfo()['relayfee'],
            node.getnetworkinfo()['relayfee'],
        )

    def run_test(self):
        seen = []
        for i, node in enumerate(self.nodes):
            expected_floor = Decimal(self.floor_sats[i]) / TBCCOIN

            mempoolminfee, info_relayfee, networkinfo_relayfee = self.relay_fees(node)

            # 1. the three are a single unified value
            assert_equal(info_relayfee, mempoolminfee)
            assert_equal(networkinfo_relayfee, mempoolminfee)

            # 2. that value is the configured mempool floor
            assert_equal(mempoolminfee, expected_floor)

            # 3. the (removed) minrelaytxfee knob is ignored
            assert mempoolminfee != IGNORED_MINRELAYTXFEE, \
                "relayfee {} unexpectedly equals the ignored -minrelaytxfee".format(mempoolminfee)

            self.log.info(
                "node{}: relayfee == mempoolminfee == {} (floor), minrelaytxfee ignored - PASS"
                .format(i, mempoolminfee))
            seen.append(mempoolminfee)

        # Different mempoolminfeerate => different relay fee, proving the relay
        # fee is driven by the mempool floor rather than a shared static value.
        assert seen[0] != seen[1], \
            "nodes with different mempoolminfeerate reported the same relayfee {}".format(seen[0])
        self.log.info("relay fee differs across nodes per their mempoolminfeerate - PASS")

if __name__ == '__main__':
    RelayFeeTracksMempoolMinFeeTest().main()
