#!/usr/bin/env python3
# Copyright (c) 2026 TBCNODE developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""Test that getdata for an invalid block hash does not terminate the node."""

import test_framework.mininode as mininode
from test_framework.mininode import CInv, NetworkThread, NodeConn, NodeConnCB, msg_getdata
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import p2p_port


class GetDataUnknownBlockTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True

    def run_test(self):
        mininode.MY_VERSION = 90014

        peer = NodeConnCB()
        conn = NodeConn("127.0.0.1", p2p_port(0), self.nodes[0], peer)
        peer.add_connection(conn)
        NetworkThread().start()
        peer.wait_for_verack()

        invalid_block_hash = int(
            "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", 16)

        self.log.info("Requesting an invalid/unknown block hash over getdata")
        peer.send_message(msg_getdata([CInv(2, invalid_block_hash)]))

        # A pong confirms the node processed the preceding getdata message and
        # remains responsive.
        peer.sync_with_ping()


if __name__ == '__main__':
    GetDataUnknownBlockTest().main()
