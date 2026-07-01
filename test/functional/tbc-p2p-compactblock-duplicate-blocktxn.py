#!/usr/bin/env python3
# Copyright (c) 2026
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test duplicate bad blocktxn handling after compact block reconstruction fails."""

from test_framework.blocktools import create_block, create_coinbase, create_tx
from test_framework.mininode import *
import test_framework.mininode as mininode
from test_framework.script import CScript, OP_TRUE
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal, p2p_port, wait_until


mininode.MY_VERSION = 90014


class CompactBlockTxnTestNode(NodeConnCB):
    pass


class CompactBlockDuplicateBlockTxnTest(BitcoinTestFramework):
    def set_test_params(self):
        self.setup_clean_chain = True
        self.num_nodes = 1

    def build_block_on_tip(self, tx=None):
        node = self.nodes[0]
        height = node.getblockcount()
        tip = node.getbestblockhash()
        mtp = node.getblockheader(tip)["mediantime"]

        block = create_block(int(tip, 16), create_coinbase(height + 1), mtp + 1)
        block.nVersion = 4
        if tx is not None:
            block.vtx.append(tx)
        block.hashMerkleRoot = block.calc_merkle_root()
        block.solve()
        return block

    def run_test(self):
        peer = CompactBlockTxnTestNode()
        conn = NodeConn("127.0.0.1", p2p_port(0), self.nodes[0], peer)
        peer.add_connection(conn)
        NetworkThread().start()
        peer.wait_for_verack()

        spend_block = self.build_block_on_tip()
        peer.send_and_ping(msg_block(spend_block))
        assert_equal(int(self.nodes[0].getbestblockhash(), 16), spend_block.sha256)
        self.nodes[0].generate(100)

        spend_tx = create_tx(spend_block.vtx[0], 0, spend_block.vtx[0].vout[0].nValue - 1000, CScript([OP_TRUE]))
        spend_tx.rehash()

        block = self.build_block_on_tip(spend_tx)

        compact_block = HeaderAndShortIDs()
        compact_block.initialize_from_block(block, prefill_list=[0])
        peer.send_and_ping(msg_cmpctblock(compact_block.to_p2p()))

        with mininode_lock:
            assert "getblocktxn" in peer.last_message
            requested = peer.last_message["getblocktxn"].block_txn_request.to_absolute()
        assert_equal(requested, [1])

        bad_blocktxn = msg_blocktxn()
        bad_blocktxn.block_transactions = BlockTransactions(block.sha256, [block.vtx[0]])

        peer.send_and_ping(bad_blocktxn)
        assert_equal(int(self.nodes[0].getbestblockhash(), 16), block.hashPrevBlock)

        wait_until(lambda: "getdata" in peer.last_message, timeout=10, lock=mininode_lock)
        with mininode_lock:
            assert_equal(len(peer.last_message["getdata"].inv), 1)
            assert_equal(peer.last_message["getdata"].inv[0].type, 2)
            assert_equal(peer.last_message["getdata"].inv[0].hash, block.sha256)

        # This is the regression trigger. A vulnerable node aborts here because
        # the first bad blocktxn left a cleared partial block reachable.
        peer.send_and_ping(bad_blocktxn)

        peer.send_and_ping(msg_block(block))
        assert_equal(int(self.nodes[0].getbestblockhash(), 16), block.sha256)


if __name__ == "__main__":
    CompactBlockDuplicateBlockTxnTest().main()
