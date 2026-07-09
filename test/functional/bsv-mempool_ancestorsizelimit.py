#!/usr/bin/env python3
# Copyright (c) 2019 Bitcoin Association
# Distributed under the Open TBC software license, see the accompanying file LICENSE.

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *
from test_framework.mininode import *
from test_framework.script import CScript, OP_TRUE, OP_RETURN

# Test that -limitancestorsize and -limitdescendantsize are retained as
# compatibility options but no longer enforce mempool admission limits. Mempool
# chain policy is based on ancestor height only.

class MempoolSizeLimitTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.limitancestorsize = 1000
        self.limitdescendantsize = 2000
        # limitancestorsize and limitdescendantsize are passed in kilobytes
        self.extra_args = [["-limitancestorsize=%d" % (self.limitancestorsize / 1000), "-limitdescendantsize=%d" % (self.limitdescendantsize / 1000)]]

    # Build and submit a transaction that spends parent_txid:vout
    # Return amount sent
    def chain_transaction(self, node, parent_txid, vout, value, num_outputs):
        fee = 300
        send_value = (value - fee) / num_outputs

        tx = CTransaction()
        tx.vin.append(CTxIn(COutPoint(parent_txid, vout), b''))
        for i in range(num_outputs):
            tx.vout.append(CTxOut(int(send_value), CScript([OP_TRUE])))
        # Append some data, so that transactions are larger
        tx.vout.append(CTxOut(int(0), CScript([OP_RETURN,  b"a" * 200])))
        tx.rehash()
        node.sendrawtransaction(ToHex(tx))
   
        txSize = len(tx.serialize())
        return (tx, send_value, txSize)

    def run_test(self):
        # 0. Prepare initial blocks.
        self.nodes[0].generate(101)

        ######################################
        # 1. Ancestor size does not reject transactions.
        # First create funding transaction that pays to output that does not require signatures.
        out_value = 10000
        ftx = CTransaction()
        ftx.vout.append(CTxOut(out_value, CScript([OP_TRUE])))
        ftxHex = self.nodes[0].fundrawtransaction(ToHex(ftx),{ 'changePosition' : len(ftx.vout)})['hex'] 
        ftxHex = self.nodes[0].signrawtransaction(ftxHex)['hex']
        ftx = FromHex(CTransaction(), ftxHex)
        ftx.rehash()
        self.nodes[0].sendrawtransaction(ftxHex)
        wait_until(lambda: ftx.hash in self.nodes[0].getrawmempool(), timeout=5)
        ancestorsSize = len(ftxHex)/2
        txid = ftx.sha256
        value = out_value

        # Build enough transactions to exceed the configured ancestor-size value
        # while staying below the default ancestor-height limit.
        MAX_ANCESTORS = 25
        exceededAncestorSize = False
        for i in range(0, MAX_ANCESTORS - 1):
            (tx, sent_value, txSize) = self.chain_transaction(self.nodes[0], txid, 0, value, 1)
            txid = tx.sha256
            value = sent_value
            ancestorsSize = ancestorsSize + txSize
            wait_until(lambda: tx.hash in self.nodes[0].getrawmempool(), timeout=5)

            if ancestorsSize > self.limitancestorsize:
                exceededAncestorSize = True
                break

        assert(exceededAncestorSize)

        ######################################
        # 2. Descendant size does not reject transactions.
        send_value = 10000
   
        ftx = CTransaction()
        MAX_DESCENDANTS = 25
        for i in range(MAX_DESCENDANTS):
            ftx.vout.append(CTxOut(send_value, CScript([OP_TRUE])))
        ftxHex = self.nodes[0].fundrawtransaction(ToHex(ftx),{ 'changePosition' : len(ftx.vout)})['hex'] 
        ftxHex = self.nodes[0].signrawtransaction(ftxHex)['hex']
        ftx = FromHex(CTransaction(), ftxHex)
        ftx.rehash()
        descendantsSize = len(ftxHex)/2
        self.nodes[0].sendrawtransaction(ftxHex)

        utxos = []
        for i in range(MAX_DESCENDANTS):
            utxos.append({'txid': ftx.sha256, 'vout': i, 'amount': send_value})

        wait_until(lambda: ftx.hash in self.nodes[0].getrawmempool(), timeout=5)

        # Build enough siblings to exceed the configured descendant-size value.
        # Siblings avoid the ancestor-height limit, isolating the deprecated
        # descendant-size option.
        exceededDescendantSize = False
        for i in range(MAX_DESCENDANTS):
            utxo = utxos.pop(0)
            (tx, sent_value, txSize) = self.chain_transaction(self.nodes[0], utxo['txid'], utxo['vout'], utxo['amount'], 1)
            descendantsSize = descendantsSize + txSize
            wait_until(lambda: tx.hash in self.nodes[0].getrawmempool(), timeout=5)
            if descendantsSize > self.limitdescendantsize:
                exceededDescendantSize = True
                break

        assert(exceededDescendantSize)

if __name__ == '__main__':
    MempoolSizeLimitTest().main()
