#!/usr/bin/env python3
# Copyright (c) 2020 Bitcoin Association
# Copyright (c) 2026 TBC Node
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
#
# Regression test for -limitancestorcount when interpreted as ancestor *height*
# (max chain depth) rather than ancestor count.
#
# Business change: ccf31423c0c07eff330ecfd1cd9f17897c3e4f9d, 3d18efafc89a1eff8efd4240fe48566b95933fb8
# Changed > to >= in CalculateMemPoolAncestorsNL, so height >= limit is rejected.

from test_framework import mininode
from test_framework.blocktools import create_block, create_coinbase
from test_framework.mininode import (
    CTransaction,
    CTxIn,
    COutPoint,
    CTxOut,
    msg_tx,
    msg_block,
)
from test_framework.script import CScript, OP_DROP, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    wait_until,
    check_mempool_equals,
    assert_equal,
    assert_raises_rpc_error,
)
from decimal import Decimal, ROUND_DOWN

# Override mininode version to match node requirements (90014+)
mininode.MY_VERSION = 90014


class MempoolAncestorHeightLimits(BitcoinTestFramework):
    """Test ancestor height limits for primary mempool."""

    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def setup_network(self):
        self.setup_nodes()

    def setup_nodes(self):
        self.add_nodes(self.num_nodes)

    def create_tx(self, outpoints, noutput, feerate):
        """Create a transaction with given inputs and outputs."""
        tx = CTransaction()
        total_input = 0
        for parent_tx, n in outpoints:
            tx.vin.append(CTxIn(COutPoint(parent_tx.sha256, n), b"", 0xffffffff))
            total_input += parent_tx.vout[n].nValue

        estimated_size = 150 + len(outpoints) * 100 + noutput * 35
        total_fee = int(estimated_size * feerate)

        output_value = (total_input - total_fee) // noutput
        assert output_value > 0, f"Output value must be positive: {output_value}"

        for _ in range(noutput):
            tx.vout.append(CTxOut(output_value, CScript([b"X"*200, OP_DROP, OP_TRUE])))

        tx.rehash()
        return tx

    def mine_transactions(self, conn, txs):
        """Mine given transactions into a block."""
        last_block_info = conn.rpc.getblock(conn.rpc.getbestblockhash())
        block = create_block(
            int(last_block_info["hash"], 16),
            coinbase=create_coinbase(height=last_block_info["height"] + 1),
            nTime=last_block_info["time"] + 1
        )
        block.vtx.extend(txs)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.calc_sha256()
        block.solve()

        conn.send_message(msg_block(block))
        wait_until(lambda: conn.rpc.getbestblockhash() == block.hash, timeout=30, check_interval=0.3)
        return block

    def _prepare_funding(self):
        """Prepare funding transactions with 4 outputs."""
        with self.run_node_with_connections(
            "Prepare funding",
            0,
            ["-blockmintxfee=0.00001", "-relayfee=0.000005"],
            number_of_connections=1
        ) as (conn,):
            coinbase = create_coinbase(height=1)
            first_block = create_block(
                int(conn.rpc.getbestblockhash(), 16),
                coinbase=coinbase
            )
            first_block.solve()
            conn.send_message(msg_block(first_block))
            wait_until(
                lambda: conn.rpc.getbestblockhash() == first_block.hash,
                timeout=30,
                check_interval=0.3
            )

            conn.rpc.generate(150)
            funding_tx = self.create_tx([(coinbase, 0)], 4, feerate=1.5)

            conn.send_message(msg_tx(funding_tx))
            check_mempool_equals(conn.rpc, [funding_tx])
            conn.rpc.generate(1)

            return funding_tx

    # ====================================================================
    # P2P Tests
    # ====================================================================

    def _test_chain_height_limit_p2p(self, outpoint):
        """P2P: Chain of 20 txs ok, 21st rejected (limitancestorcount=20)."""
        limitancestorcount = 20

        with self.run_node_with_connections(
            "P2P chain height limit",
            0,
            [
                "-blockmintxfee=0.00001",
                "-relayfee=0.000005",
                f"-limitancestorcount={limitancestorcount}",
                "-checkmempool=1",
            ],
            number_of_connections=1
        ) as (conn,):
            rejected_txs = []
            def on_reject(conn, msg):
                rejected_txs.append(msg)
            conn.cb.on_reject = on_reject

            mining_fee = 1.001

            # Create chain of transactions (height 0 to 20)
            last_outpoint = outpoint
            chain = []
            for _ in range(limitancestorcount + 1):
                tx = self.create_tx([last_outpoint], 1, mining_fee)
                chain.append(tx)
                last_outpoint = (tx, 0)

            # Send first 20 transactions (height 0-19, within limit)
            for tx in chain[:-1]:
                conn.send_message(msg_tx(tx))

            check_mempool_equals(conn.rpc, chain[:-1])

            # 21st transaction (height 20, exceeds limit) should be rejected
            conn.send_message(msg_tx(chain[-1]))
            wait_until(lambda: len(rejected_txs) == 1, timeout=30, check_interval=0.3)
            assert_equal(rejected_txs[0].data, chain[-1].sha256)
            assert_equal(rejected_txs[0].reason, b'too-long-mempool-chain')

            # Mine the first transaction to shorten the chain
            self.mine_transactions(conn, [chain[0]])

            # Now chain[-1] should be accepted (height is now 19)
            conn.send_message(msg_tx(chain[-1]))
            check_mempool_equals(conn.rpc, chain[1:])

            # Clean up
            self.mine_transactions(conn, chain[1:])

    def _test_graph_height_limit_p2p(self, outpoint):
        """P2P: DAG with tx6 rejected (ancestor height 4 > limit 3)."""
        limitancestorcount = 3

        with self.run_node_with_connections(
            "P2P graph height limit",
            0,
            [
                "-blockmintxfee=0.00001",
                "-relayfee=0.000005",
                f"-limitancestorcount={limitancestorcount}",
                "-checkmempool=1",
            ],
            number_of_connections=1
        ) as (conn,):
            rejected_txs = []
            def on_reject(conn, msg):
                rejected_txs.append(msg)
            conn.cb.on_reject = on_reject

            mining_fee = 1.001

            # Create graph:
            #                    tx1 (height 0)
            #          +---------+    |    +---------+
            #          |              |              |
            #        tx2 (h=1)    tx3 (h=1)    tx4 (h=1)
            #          |              |              |
            #          +---------+    |    +---------+
            #                    tx5 (height 2)
            #                      |
            #                    tx6 (height 3) <- should be rejected

            tx1 = self.create_tx([outpoint], 3, mining_fee)
            tx2 = self.create_tx([(tx1, 0)], 1, mining_fee)
            tx3 = self.create_tx([(tx1, 1)], 1, mining_fee)
            tx4 = self.create_tx([(tx1, 2)], 1, mining_fee)
            tx5 = self.create_tx([(tx2, 0), (tx3, 0), (tx4, 0)], 1, mining_fee)
            tx6 = self.create_tx([(tx5, 0)], 1, mining_fee)

            for tx in [tx1, tx2, tx3, tx4, tx5]:
                conn.send_message(msg_tx(tx))

            check_mempool_equals(conn.rpc, [tx1, tx2, tx3, tx4, tx5])

            # tx6 should be rejected (ancestor height = 4 > limit = 3)
            conn.send_message(msg_tx(tx6))
            wait_until(lambda: len(rejected_txs) == 1, timeout=30, check_interval=0.3)
            assert_equal(rejected_txs[0].data, tx6.sha256)
            assert_equal(rejected_txs[0].reason, b'too-long-mempool-chain')

            # Mine tx1 to shorten the chain
            self.mine_transactions(conn, [tx1])

            # Now tx6 should be accepted (ancestor height is now 3)
            conn.send_message(msg_tx(tx6))
            check_mempool_equals(conn.rpc, [tx2, tx3, tx4, tx5, tx6])

            # Clean up
            self.mine_transactions(conn, [tx2, tx3, tx4, tx5, tx6])

    def _test_wide_dag_accepted_p2p(self, outpoint):
        """P2P: Wide DAG accepted (height 2 < limit 3)."""
        limitancestorcount = 3

        with self.run_node_with_connections(
            "P2P wide DAG accepted",
            0,
            [
                "-blockmintxfee=0.00001",
                "-relayfee=0.000005",
                f"-limitancestorcount={limitancestorcount}",
                "-checkmempool=1",
            ],
            number_of_connections=1
        ) as (conn,):
            mining_fee = 1.001

            tx1 = self.create_tx([outpoint], 3, mining_fee)
            tx2 = self.create_tx([(tx1, 0)], 1, mining_fee)
            tx3 = self.create_tx([(tx1, 1)], 1, mining_fee)
            tx4 = self.create_tx([(tx1, 2)], 1, mining_fee)
            tx5 = self.create_tx([(tx2, 0), (tx3, 0), (tx4, 0)], 1, mining_fee)

            for tx in [tx1, tx2, tx3, tx4, tx5]:
                conn.send_message(msg_tx(tx))

            check_mempool_equals(conn.rpc, [tx1, tx2, tx3, tx4, tx5])

            self.mine_transactions(conn, [tx1, tx2, tx3, tx4, tx5])

    # ====================================================================
    # RPC Tests
    # ====================================================================

    def _rpc_chain_transaction(self, node, parent_txid, vout, value, fee, num_outputs):
        """Build, sign and send one tx via RPC."""
        value = Decimal(str(value))
        fee = Decimal(str(fee))
        total_value = value - fee
        send_value = (total_value / num_outputs).quantize(Decimal("0.000001"), rounding=ROUND_DOWN)

        inputs = [{"txid": parent_txid, "vout": vout}]
        outputs = {node.getnewaddress(): float(send_value) for _ in range(num_outputs)}

        rawtx = node.createrawtransaction(inputs, outputs)
        signed = node.signrawtransaction(rawtx)
        txid = node.sendrawtransaction(signed["hex"])
        return (txid, float(send_value))

    def _test_chain_height_limit_rpc(self):
        """RPC: Chain of 20 txs ok, 21st rejected (limitancestorcount=20)."""
        limit = 20
        self.start_node(0, [
            "-blockmintxfee=0.00001",
            "-relayfee=0.000005",
            f"-limitancestorcount={limit}",
            "-checkmempool=1",
        ])
        node = self.nodes[0]

        node.generate(101)
        utxo = node.listunspent(1)[0]

        funding_txid = utxo["txid"]
        value = Decimal(str(utxo["amount"]))
        fee = Decimal("0.0001")

        # Create chain of 20 txs
        chain = []
        parent_txid = funding_txid
        for i in range(limit):
            parent_txid, value = self._rpc_chain_transaction(
                node, parent_txid, 0 if i > 0 else utxo["vout"], value, fee, 1
            )
            chain.append(parent_txid)

        assert_equal(len(node.getrawmempool()), limit)

        # 21st should be rejected
        def send_21st():
            self._rpc_chain_transaction(node, chain[-1], 0, value, fee, 1)

        assert_raises_rpc_error(-26, "too-long-mempool-chain", send_21st)

        node.generate(1)
        assert_equal(len(node.getrawmempool()), 0)
        self.stop_node(0)

    def _test_graph_height_limit_rpc(self):
        """RPC: DAG with tx6 rejected (ancestor height 4 > limit 3)."""
        self.start_node(0, [
            "-blockmintxfee=0.00001",
            "-relayfee=0.000005",
            "-limitancestorcount=3",
            "-checkmempool=1",
        ])
        node = self.nodes[0]

        node.generate(101)
        utxo = node.listunspent(1)[0]

        funding_txid = utxo["txid"]
        value = Decimal(str(utxo["amount"]))
        fee = Decimal("0.0001")

        tx1_id, v1 = self._rpc_chain_transaction(node, funding_txid, utxo["vout"], value, fee, 3)
        tx2_id, v2 = self._rpc_chain_transaction(node, tx1_id, 0, v1, fee, 1)
        tx3_id, v3 = self._rpc_chain_transaction(node, tx1_id, 1, v1, fee, 1)
        tx4_id, v4 = self._rpc_chain_transaction(node, tx1_id, 2, v1, fee, 1)

        inputs = [{"txid": tx2_id, "vout": 0}, {"txid": tx3_id, "vout": 0}, {"txid": tx4_id, "vout": 0}]
        combined = (Decimal(str(v2 + v3 + v4)) - fee).quantize(Decimal("0.000001"), rounding=ROUND_DOWN)
        outputs = {node.getnewaddress(): float(combined)}
        raw5 = node.createrawtransaction(inputs, outputs)
        tx5_id = node.sendrawtransaction(node.signrawtransaction(raw5)["hex"])

        # tx6 should be rejected
        def send_tx6():
            self._rpc_chain_transaction(node, tx5_id, 0, float(combined), fee, 1)

        assert_raises_rpc_error(-26, "too-long-mempool-chain", send_tx6)

        node.generate(1)
        assert_equal(len(node.getrawmempool()), 0)
        self.stop_node(0)

    def _test_wide_dag_accepted_rpc(self):
        """RPC: Wide DAG accepted (height 2 < limit 3)."""
        self.start_node(0, [
            "-blockmintxfee=0.00001",
            "-relayfee=0.000005",
            "-limitancestorcount=3",
            "-checkmempool=1",
        ])
        node = self.nodes[0]

        node.generate(101)
        utxo = node.listunspent(1)[0]

        funding_txid = utxo["txid"]
        value = Decimal(str(utxo["amount"]))
        fee = Decimal("0.0001")

        tx1_id, v1 = self._rpc_chain_transaction(node, funding_txid, utxo["vout"], value, fee, 3)
        tx2_id, v2 = self._rpc_chain_transaction(node, tx1_id, 0, v1, fee, 1)
        tx3_id, v3 = self._rpc_chain_transaction(node, tx1_id, 1, v1, fee, 1)
        tx4_id, v4 = self._rpc_chain_transaction(node, tx1_id, 2, v1, fee, 1)

        inputs = [{"txid": tx2_id, "vout": 0}, {"txid": tx3_id, "vout": 0}, {"txid": tx4_id, "vout": 0}]
        combined = (Decimal(str(v2 + v3 + v4)) - fee).quantize(Decimal("0.000001"), rounding=ROUND_DOWN)
        outputs = {node.getnewaddress(): float(combined)}
        raw5 = node.createrawtransaction(inputs, outputs)
        node.sendrawtransaction(node.signrawtransaction(raw5)["hex"])

        assert_equal(len(node.getrawmempool()), 5)

        node.generate(1)
        assert_equal(len(node.getrawmempool()), 0)
        self.stop_node(0)

    # ====================================================================
    # Main Test Runner
    # ====================================================================

    def run_test(self):
        funding_tx = self._prepare_funding()

        # P2P tests
        self._test_chain_height_limit_p2p((funding_tx, 0))
        self._test_graph_height_limit_p2p((funding_tx, 1))
        self._test_wide_dag_accepted_p2p((funding_tx, 2))

        # RPC tests
        self._test_chain_height_limit_rpc()
        self._test_graph_height_limit_rpc()
        self._test_wide_dag_accepted_rpc()


if __name__ == '__main__':
    MempoolAncestorHeightLimits().main()
