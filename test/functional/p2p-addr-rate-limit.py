#!/usr/bin/env python3
# Copyright (c) 2026 The TBC developers
# Distributed under the Open TBC software license, see the accompanying file LICENSE.

"""Test per-peer rate limiting of incoming ADDR records."""

import time

import test_framework.mininode as mininode
from test_framework.mininode import (
    CAddress,
    NetworkThread,
    NodeConn,
    NodeConnCB,
    msg_addr,
)
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import check_for_log_msg, p2p_port, wait_until


class AddrRateLimitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-allowunsolicitedaddr=1", "-debug=net"]]

    def make_addr_message(self, first_octet, count):
        message = msg_addr()
        for last_octet in range(1, count + 1):
            message.addrs.append(
                CAddress("{}.1.1.{}".format(first_octet, last_octet), 8333)
            )
        return message

    def wait_for_log(self, message):
        wait_until(
            lambda: check_for_log_msg(self, message, "/node0"),
            timeout=10,
        )

    def run_test(self):
        mininode.MY_VERSION = self.nodes[0].getnetworkinfo()["protocolversion"]

        peer = NodeConnCB()
        connection = NodeConn(
            "127.0.0.1", p2p_port(0), self.nodes[0], peer
        )
        peer.add_connection(connection)
        NetworkThread().start()
        peer.wait_for_verack()

        self.log.info("A new peer can process one address immediately")
        peer.send_and_ping(self.make_addr_message(101, 10))
        self.wait_for_log(
            "Received addr: 10 addresses (1 processed, 9 rate-limited)"
        )

        self.log.info("The peer earns one additional token after ten seconds")
        time.sleep(11)
        peer.send_and_ping(self.make_addr_message(102, 1))
        self.wait_for_log(
            "Received addr: 1 addresses (1 processed, 0 rate-limited)"
        )

        self.log.info("The newly earned token was consumed")
        peer.send_and_ping(self.make_addr_message(103, 1))
        self.wait_for_log(
            "Received addr: 1 addresses (0 processed, 1 rate-limited)"
        )


if __name__ == "__main__":
    AddrRateLimitTest().main()
