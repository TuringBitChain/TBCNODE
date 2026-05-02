#!/usr/bin/env python3
# Functional test for the cs_main-free getrawtransactiondata RPC.
#
# Verifies:
#   1. mempool path returns the same hex as getrawtransaction(verbose=false)
#   2. on-disk (txindex) path returns the same hex after the tx is mined
#   3. unknown txid produces the correct error
#   4. -txindex disabled + on-disk lookup yields the indexed message

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    assert_raises_rpc_error,
)


class GetRawTransactionDataTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        # node0: -txindex on (full path)
        # node1: -txindex off (mempool-only path)
        self.extra_args = [["-txindex"], []]

    def run_test(self):
        node_indexed = self.nodes[0]
        node_no_index = self.nodes[1]

        # Generate spendable coins.
        node_indexed.generate(101)
        self.sync_all()

        # Create a transaction and submit (still in mempool).
        addr = node_indexed.getnewaddress()
        utxos = node_indexed.listunspent()
        assert utxos, "expected coinbase utxo"
        u = utxos[0]
        raw = node_indexed.createrawtransaction(
            [{"txid": u["txid"], "vout": u["vout"]}],
            {addr: float(u["amount"]) - 0.001},
        )
        signed = node_indexed.signrawtransaction(raw)["hex"]
        txid = node_indexed.sendrawtransaction(signed)
        self.sync_all()

        # 1. mempool path: hex must match getrawtransaction(verbose=false).
        legacy_hex = node_indexed.getrawtransaction(txid, False)
        fast_hex = node_indexed.getrawtransactiondata(txid)
        assert_equal(legacy_hex, fast_hex)
        self.log.info("mempool path matches getrawtransaction")

        # node1 is also in mempool here (via P2P), so fast path should work
        # without -txindex.
        fast_hex_no_index = node_no_index.getrawtransactiondata(txid)
        assert_equal(legacy_hex, fast_hex_no_index)
        self.log.info("mempool path works without -txindex")

        # 2. mine the tx; lookup must now hit txindex on node0 and fail on node1.
        node_indexed.generate(1)
        self.sync_all()

        legacy_hex_mined = node_indexed.getrawtransaction(txid, False)
        fast_hex_mined = node_indexed.getrawtransactiondata(txid)
        assert_equal(legacy_hex_mined, fast_hex_mined)
        assert_equal(legacy_hex, legacy_hex_mined)
        self.log.info("txindex/on-disk path matches getrawtransaction")

        # node1 has no -txindex, no mempool entry: must error.
        assert_raises_rpc_error(
            -5,
            "No such mempool transaction",
            node_no_index.getrawtransactiondata,
            txid,
        )
        self.log.info("missing -txindex produces correct error")

        # 3. unknown txid → error.
        bogus = "00" * 32
        assert_raises_rpc_error(
            -5,
            "No such mempool or blockchain transaction",
            node_indexed.getrawtransactiondata,
            bogus,
        )
        self.log.info("unknown txid produces correct error")


if __name__ == "__main__":
    GetRawTransactionDataTest().main()
