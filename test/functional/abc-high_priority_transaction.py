#!/usr/bin/env python3
# Copyright (c) 2017 The Bitcoin developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.

#
# Test HighPriorityTransaction code
#

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *


class HighPriorityTransactionTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        self.extra_args = [["-blockprioritypercentage=0", "-limitfreerelay=2", "-blockassembler=legacy"]]

    def run_test(self):
        # Legacy priority-area block assembly has been removed. The legacy
        # assembler option and blockprioritypercentage are accepted for
        # compatibility and fall back to the journaling assembler.
        self.nodes[0].generate(1)

        self.stop_nodes()
        self.nodes = []
        self.add_nodes(self.num_nodes, [["-limitfreerelay=2", "-blockassembler=legacy"]])
        self.start_nodes()
        self.nodes[0].generate(1)


if __name__ == '__main__':
    HighPriorityTransactionTest().main()
