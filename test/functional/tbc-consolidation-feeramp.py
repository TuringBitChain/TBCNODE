#!/usr/bin/env python3
# Copyright (c) 2020 Bitcoin Association
# Distributed under the Open TBC software license, see the accompanying file LICENSE.

"""
Verify that the free-consolidation subsidy only applies while the mempool is in
the flat-floor stage (dynamic memory usage <= -mempoolfeerampstart, N1).

A zero-fee consolidation transaction is admitted for free while usage <= N1,
because it receives a fee subsidy of blockmintxfee * size. Once usage crosses
N1 and the admission feerate starts ramping, the subsidy is no longer granted,
so a zero-fee consolidation fails the mempool minimum fee check. A consolidation
paying enough voluntary fee would still be admitted, but that is covered by the
existing consolidation tests; here we focus on the zero-fee case being rejected
in the ramp stage.

Notes:
 - This chain uses TBCCOIN (1e6) base units per coin, so amounts are scaled with
   TBCCOIN here rather than the mininode COIN (1e8) constant.
 - Outpoints are resolved via RPC (getrawtransaction verbose) rather than by
   recomputing txids with mininode, whose hashing does not match this chain.
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_raises_rpc_error, satoshi_round, assert_greater_than
from decimal import Decimal

# Base monetary units per coin on this chain (src/amount.h: TBCCOIN).
TBCCOIN = 1000000

class ConsolidationFeeRampTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.utxo_test_sats = 10000
        self.utxo_test_coins = satoshi_round(Decimal(self.utxo_test_sats) / TBCCOIN)
        # The consolidation subsidy rate is the mempool block-min-tx-fee, set by
        # -blockmintxfee. For a zero-fee consolidation to be admitted in the
        # flat-floor stage, the subsidy must cover the floor, so blockmintxfee
        # must exceed mempoolminfeerate. (CFeeRate::GetFee clamps size up to 1000
        # bytes, so for these small txns the fee equals the per-kB rate directly.)
        # Note: mempoolminfeerate is deliberately set above DEFAULT_BLOCK_MIN_TX_FEE
        # (60) but below blockmintxfee (100). This way the flat-stage admission
        # only succeeds if the subsidy actually tracks -blockmintxfee; if it
        # regressed to the old fixed config default (60), stage 1 would fail.
        self.blockmintxfee_sats = 100
        self.mempoolminfeerate_sats = 80
        # Keep the flat-floor stage tiny so a couple of transactions push the
        # mempool usage past N1 into the ramp.
        self.rampstart_bytes = 1000
        self.extra_args = [[
            "-whitelist=127.0.0.1",
            "-mempoolminfeerate={}".format(self.mempoolminfeerate_sats),
            "-mempoolfeerampstart={}B".format(self.rampstart_bytes),
            # ParseMoney enforces this chain's 6-decimal precision, so pass a
            # plain (non zero-padded) decimal string for blockmintxfee.
            "-blockmintxfee={}".format(Decimal(self.blockmintxfee_sats) / TBCCOIN),
            "-minconsolidationfactor=2",
            "-acceptnonstdtxn=1",
            "-txindex=1",
        ]]

    # Create in_count confirmed utxos each worth utxo_test_coins, paid to a fresh
    # wallet address. Returns the list of {txid, vout} outpoints (still in the
    # mempool; the caller is responsible for maturing them).
    def create_outpoints(self, node, in_count):
        addr = node.getnewaddress()
        outpoints = []
        for _ in range(in_count):
            txid = node.sendtoaddress(addr, self.utxo_test_coins)
            verbose = node.getrawtransaction(txid, 1)
            vout = next(o['n'] for o in verbose['vout']
                        if Decimal(str(o['value'])) == self.utxo_test_coins)
            outpoints.append({"txid": txid, "vout": vout})
        return outpoints

    # Build a signed zero-fee consolidation: spend all outpoints into a single
    # output of the same total value (no fee).
    def build_zero_fee_consolidation(self, node, outpoints):
        sum_values = len(outpoints) * self.utxo_test_coins
        addr = node.getnewaddress()
        rawtx = node.createrawtransaction(outpoints, {addr: sum_values})
        signed = node.signrawtransaction(rawtx)
        assert signed['complete']
        return signed['hex']

    # Pump the mempool with fee-paying wallet transactions until its dynamic
    # memory usage exceeds the ramp start (N1). Does not mine blocks.
    def pump_usage_above_rampstart(self, node, rampstart):
        for _ in range(50):
            if node.getmempoolinfo()['usage'] > rampstart:
                return
            node.sendtoaddress(node.getnewaddress(), self.utxo_test_coins)
        assert_greater_than(node.getmempoolinfo()['usage'], rampstart)

    def run_test(self):
        node = self.nodes[0]
        self.consolidation_factor = int(node.getnetworkinfo()['minconsolidationfactor'])
        self.minConfirmations = int(node.getnetworkinfo()['minconsolidationinputmaturity'])
        rampstart = node.getmempoolinfo()['mempoolfeerampstart']
        self.log.info ("consolidation factor: {}".format(self.consolidation_factor))
        self.log.info ("minimum input confirmations: {}".format(self.minConfirmations))
        self.log.info ("mempoolfeerampstart (N1): {} bytes".format(rampstart))

        node.generate(300)

        # Inputs and outputs are standard p2pkh (equal scriptPubKey sizes), so a
        # transaction with >= consolidation_factor inputs to a single output
        # satisfies both consolidation rules (input count and scriptPubKey-size
        # ratio). Add a margin so we stay clear of the threshold.
        enough_inputs = max(2, self.consolidation_factor + 1)

        # Prepare the inputs for both consolidations up front and mature them in
        # a single batch. We must not mine again after this point, otherwise the
        # mempool would be confirmed away and usage would drop back below N1.
        op_stage1 = self.create_outpoints(node, enough_inputs)
        op_ramp = self.create_outpoints(node, enough_inputs)
        node.generate(self.minConfirmations)

        # Lock the ramp-stage inputs so the usage-pumping transactions below do
        # not spend them out from under the second consolidation.
        node.lockunspent(False, op_ramp)

        cons_stage1 = self.build_zero_fee_consolidation(node, op_stage1)
        cons_ramp = self.build_zero_fee_consolidation(node, op_ramp)

        # STAGE 1 (flat floor): mempool is empty, so usage <= N1. A zero-fee
        # consolidation gets the subsidy and is admitted for free.
        assert node.getmempoolinfo()['usage'] <= rampstart
        txid = node.sendrawtransaction(cons_stage1)
        assert txid in node.getrawmempool()
        self.log.info ("stage 1 (usage <= N1): zero-fee consolidation admitted - PASS")

        # Cross N1 into the ramp.
        self.pump_usage_above_rampstart(node, rampstart)
        assert_greater_than(node.getmempoolinfo()['usage'], rampstart)

        # RAMP (usage > N1): the subsidy is no longer granted, so a zero-fee
        # consolidation now fails the mempool minimum fee check.
        assert_raises_rpc_error(-26, "66: mempool min fee not met", node.sendrawtransaction, cons_ramp)
        self.log.info ("stage ramp (usage > N1): zero-fee consolidation rejected - PASS")

if __name__ == '__main__':
    ConsolidationFeeRampTest().main()
