#!/usr/bin/env python3
# Copyright (c) 2020 Bitcoin Association
# Distributed under the Open TBC software license, see the accompanying file LICENSE.

"""
Verify the no-trim mempool-full admission policy.

With the no-trim policy the mempool is never evicted. Admission is hard-rejected
once dynamic memory usage reaches the cap (-maxmempool, N2): GetMinFee clamps and
validation rejects with "mempool full". Crucially, unlike the historical
TrimToSize behaviour, a full mempool does NOT evict low-fee transactions to make
room - not even for a higher-fee transaction.

This test:
 1. fills the mempool past -maxmempool with large transactions,
 2. asserts the next transaction is rejected with "mempool full",
 3. asserts a *high-fee* transaction is also rejected (no eviction / no-trim),
 4. asserts the mempool contents did not shrink (nothing was trimmed).

The fee ramp is disabled here (mempoolfeerampstart == maxmempool, a degenerate
N1 >= N2 config that keeps the floor flat) so admission is gated purely by the
hard size cap, not by a rising ramp fee.

Note: -maxmempool has a lower bound of 0.3 * DEFAULT_MAX_MEMPOOL_SIZE (=300MB
here), so this is a deliberately heavy test that moves a few hundred MB of txns.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, assert_greater_than, assert_raises_rpc_error
from test_framework.authproxy import JSONRPCException
from decimal import Decimal

# 300 MB: the minimum -maxmempool the node accepts (0.3 * 1000MB default).
MAXMEMPOOL = 300 * 1000 * 1000
# Each filler carries ~10 MB of OP_RETURN data.
FILLER_DATA_BYTES = 10 * 1000 * 1000

class MempoolFullNoTrimTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        # These size args go through GetArgAsBytes (default multiplier is MB), so
        # a "B" suffix is required to pass exact byte counts.
        self.extra_args = [[
            "-maxmempool={}B".format(MAXMEMPOOL),
            # Disable the ramp: N1 == N2 -> degenerate, flat floor all the way to
            # the cap, so only the hard "mempool full" gate rejects.
            "-mempoolfeerampstart={}B".format(MAXMEMPOOL),
            "-mempoolminfeerate=50",
            "-datacarriersize={}B".format(MAXMEMPOOL + FILLER_DATA_BYTES),
            "-maxtxsizepolicy=0",            # unlimited policy tx size
            "-acceptnonstdtxn=1",
            "-genesisactivationheight=1",
        ]]

    def make_big_tx(self, node, fee_coins=Decimal("1")):
        # Build a large transaction manually (one confirmed input -> a change
        # output plus a big OP_RETURN) and sign it. We bypass the wallet's
        # CreateTransaction path on purpose: it dust-rejects the value-0
        # OP_RETURN output ("Transaction amount too small"), whereas raw
        # construction + sendrawtransaction goes straight through mempool
        # admission, which accepts data-carrier outputs.
        u = next(x for x in node.listunspent()
                 if x['amount'] > fee_coins + Decimal("1"))
        change_addr = node.getnewaddress()
        change = u['amount'] - fee_coins   # input - change == fee
        raw = node.createrawtransaction(
            [{"txid": u['txid'], "vout": u['vout']}],
            {change_addr: change, "data": "00" * FILLER_DATA_BYTES})
        signed = node.signrawtransaction(raw)
        assert signed['complete']
        return signed['hex']

    def run_test(self):
        node = self.nodes[0]
        maxmempool = node.getmempoolinfo()['maxmempool']
        assert_equal(maxmempool, MAXMEMPOOL)
        self.log.info("maxmempool = {} bytes".format(maxmempool))

        node.generate(300)

        # 1. Fill until a send is rejected with "mempool full".
        full_msg = None
        for i in range(200):  # safety bound
            usage = node.getmempoolinfo()['usage']
            if usage >= maxmempool:
                break
            try:
                node.sendrawtransaction(self.make_big_tx(node))
            except JSONRPCException as e:
                full_msg = str(e)
                self.log.info("send rejected at i={}: {}".format(i, full_msg))
                break
            if i % 5 == 0:
                self.log.info("filling: usage={} / {}".format(
                    node.getmempoolinfo()['usage'], maxmempool))

        info = node.getmempoolinfo()
        assert_greater_than(info['usage'], maxmempool - FILLER_DATA_BYTES)
        self.log.info("mempool full: size={} usage={}".format(info['size'], info['usage']))

        # If the fill loop exited because usage reached the cap (rather than on a
        # rejection), the next send must now be rejected.
        if full_msg is None:
            assert node.getmempoolinfo()['usage'] >= maxmempool
            assert_raises_rpc_error(-26, "mempool full",
                                    node.sendrawtransaction, self.make_big_tx(node))
        else:
            assert "mempool full" in full_msg, full_msg

        size_when_full = node.getmempoolinfo()['size']

        # 2/3. A high-fee transaction is ALSO rejected - no eviction to make room.
        assert_raises_rpc_error(-26, "mempool full",
                                node.sendrawtransaction, self.make_big_tx(node, fee_coins=Decimal("5")))

        # 4. Nothing was trimmed: contents did not shrink.
        assert_equal(node.getmempoolinfo()['size'], size_when_full)
        self.log.info("no-trim confirmed: full mempool rejects both low- and high-fee txns, size unchanged - PASS")

if __name__ == '__main__':
    MempoolFullNoTrimTest().main()
