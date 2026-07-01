#!/usr/bin/env python3
# Copyright (c) 2014-2016 The Bitcoin Core developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
"""Test mempool ancestor/descendant queries and unconfirmed-chain limits."""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import *

MAX_ANCESTORS = 25
MAX_DESCENDANTS = 25
TBCCOIN = 1000000


def tbc_round(amount):
    return Decimal(amount).quantize(Decimal('0.000001'), rounding=ROUND_DOWN)


class MempoolPackagesTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 2
        self.extra_args = [["-maxorphantx=1000", "-checkmempool=0"],
                           ["-maxorphantx=1000", "-checkmempool=0",
                            "-limitancestorcount=5"]]

    # Build a transaction that spends parent_txid:vout
    # Return amount sent
    def chain_transaction(self, node, parent_txid, vout, value, fee, num_outputs):
        send_value = tbc_round((value - fee) / num_outputs)
        inputs = [{'txid': parent_txid, 'vout': vout}]
        outputs = {}
        for i in range(num_outputs):
            outputs[node.getnewaddress()] = send_value
        rawtx = node.createrawtransaction(inputs, outputs)
        signedtx = node.signrawtransaction(rawtx)
        txid = node.sendrawtransaction(signedtx['hex'])
        fulltx = node.getrawtransaction(txid, 1)
        # make sure we didn't generate a change output
        assert(len(fulltx['vout']) == num_outputs)
        return (txid, send_value)

    def assert_removed_package_fields(self, entry):
        removed_fields = [
            'ancestorcount',
            'ancestorsize',
            'ancestorfees',
            'descendantcount',
            'descendantsize',
            'descendantfees',
        ]
        for field in removed_fields:
            assert(field not in entry)

    def run_test(self):
        ''' Mine some blocks and have them mature. '''
        self.nodes[0].generate(101)
        utxo = self.nodes[0].listunspent(10)
        txid = utxo[0]['txid']
        vout = utxo[0]['vout']
        value = utxo[0]['amount']

        fee = Decimal("0.0001")
        # MAX_ANCESTORS transactions off a confirmed tx should be fine
        chain = []
        for i in range(MAX_ANCESTORS):
            (txid, sent_value) = self.chain_transaction(
                self.nodes[0], txid, 0, value, fee, 1)
            value = sent_value
            chain.append(txid)

        # Check mempool has MAX_ANCESTORS transactions in it. The ancestor and
        # descendant aggregate fields were removed from verbose mempool RPCs;
        # dynamic ancestor/descendant query RPCs should still work.
        mempool = self.nodes[0].getrawmempool(True)
        assert_equal(len(mempool), MAX_ANCESTORS)

        descendants = []
        ancestors = list(chain)
        for x in reversed(chain):
            # Check that getmempoolentry is consistent with getrawmempool
            entry = self.nodes[0].getmempoolentry(x)
            assert_equal(entry, mempool[x])
            self.assert_removed_package_fields(entry)
            assert_equal(mempool[x]['modifiedfee'], mempool[x]['fee'])

            # Check that getmempooldescendants is correct
            assert_equal(sorted(descendants), sorted(
                self.nodes[0].getmempooldescendants(x)))
            descendants.append(x)

            # Check that getmempoolancestors is correct
            ancestors.remove(x)
            assert_equal(sorted(ancestors), sorted(
                self.nodes[0].getmempoolancestors(x)))

        # Check that getmempoolancestors/getmempooldescendants correctly handle verbose=true
        v_ancestors = self.nodes[0].getmempoolancestors(chain[-1], True)
        assert_equal(len(v_ancestors), len(chain) - 1)
        for x in v_ancestors.keys():
            assert_equal(mempool[x], v_ancestors[x])
        assert(chain[-1] not in v_ancestors.keys())

        v_descendants = self.nodes[0].getmempooldescendants(chain[0], True)
        assert_equal(len(v_descendants), len(chain) - 1)
        for x in v_descendants.keys():
            assert_equal(mempool[x], v_descendants[x])
        assert(chain[0] not in v_descendants.keys())

        # Check that fee deltas still update the transaction's modified fee.
        self.nodes[0].prioritisetransaction(chain[0], 0, 1000)
        mempool = self.nodes[0].getrawmempool(True)
        assert_equal(mempool[chain[0]]['modifiedfee'],
                     mempool[chain[0]]['fee'] + Decimal(1000) / TBCCOIN)
        self.assert_removed_package_fields(mempool[chain[0]])

        # Undo the prioritisetransaction for later tests
        self.nodes[0].prioritisetransaction(chain[0], 0, -1000)

        self.nodes[0].prioritisetransaction(chain[-1], 0, 1000)
        mempool = self.nodes[0].getrawmempool(True)
        assert_equal(mempool[chain[-1]]['modifiedfee'],
                     mempool[chain[-1]]['fee'] + Decimal(1000) / TBCCOIN)
        self.assert_removed_package_fields(mempool[chain[-1]])

        # Check that prioritising a tx before it's added to the mempool works
        # First clear the mempool by mining a block.
        self.nodes[0].generate(1)
        sync_blocks(self.nodes)
        assert_equal(len(self.nodes[0].getrawmempool()), 0)
        # Prioritise a transaction that has been mined, then add it back to the
        # mempool by using invalidateblock.
        self.nodes[0].prioritisetransaction(chain[-1], 0, 2000)
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
        # Keep node1's tip synced with node0
        self.nodes[1].invalidateblock(self.nodes[1].getbestblockhash())

        # Now check that the transaction is in the mempool, with the right modified fee
        mempool = self.nodes[0].getrawmempool(True)

        for x in reversed(chain):
            if (x == chain[-1]):
                assert_equal(mempool[x]['modifiedfee'],
                             mempool[x]['fee'] + Decimal(2000) / TBCCOIN)
            self.assert_removed_package_fields(mempool[x])

        # TODO: check that node1's mempool is as expected

        # TODO: test ancestor size limits

        # Descendant count limits are no longer enforced. Ancestor height is
        # still enforced, so use a fan-out package that grows descendants
        # without creating a chain deeper than the ancestor limit.
        descendant_utxo = max(self.nodes[0].listunspent(1),
                              key=lambda u: u['amount'])
        txid = descendant_utxo['txid']
        value = descendant_utxo['amount']
        vout = descendant_utxo['vout']

        transaction_package = []
        # First create one parent tx with enough outputs for many direct
        # descendants.
        (txid, sent_value) = self.chain_transaction(
            self.nodes[0], txid, vout, value, fee, MAX_DESCENDANTS + 1)
        parent_transaction = txid
        for i in range(MAX_DESCENDANTS + 1):
            transaction_package.append(
                {'txid': txid, 'vout': i, 'amount': sent_value})

        # Sign and send up to the legacy descendant limit as direct children of
        # the parent transaction.
        for i in range(MAX_DESCENDANTS - 1):
            utxo = transaction_package.pop(0)
            (txid, sent_value) = self.chain_transaction(
                self.nodes[0], utxo['txid'], utxo['vout'], utxo['amount'], fee, 1)

        assert_equal(len(self.nodes[0].getmempooldescendants(parent_transaction)),
                     MAX_DESCENDANTS - 1)

        # Sending one more descendant succeeds because descendant count limits
        # are no longer enforced.
        utxo = transaction_package.pop(0)
        (txid, sent_value) = self.chain_transaction(
            self.nodes[0], utxo['txid'], utxo['vout'], utxo['amount'], fee, 1)
        assert(txid in self.nodes[0].getrawmempool())
        assert_equal(len(self.nodes[0].getmempooldescendants(parent_transaction)),
                     MAX_DESCENDANTS)

        # TODO: check that node1's mempool is as expected

        # TODO: test descendant size limits

        # Test reorg handling
        # First, the basics:
        self.nodes[0].generate(1)
        sync_blocks(self.nodes)
        self.nodes[1].invalidateblock(self.nodes[0].getbestblockhash())
        self.nodes[1].reconsiderblock(self.nodes[0].getbestblockhash())

        # Now test the case where node1 has a transaction T in its mempool that
        # depends on transactions A and B which are in a mined block, and the
        # block containing A and B is disconnected, AND B is not accepted back
        # into node1's mempool because its ancestor count is too high.

        # Create 8 transactions, like so:
        # Tx0 -> Tx1 (vout0)
        #   \--> Tx2 (vout1) -> Tx3 -> Tx4 -> Tx5 -> Tx6 -> Tx7
        #
        # Mine them in the next block, then generate a new tx8 that spends
        # Tx1 and Tx7, and add to node1's mempool, then disconnect the
        # last block.

        # Create tx0 with 2 outputs
        utxo = self.nodes[0].listunspent()
        txid = utxo[0]['txid']
        value = utxo[0]['amount']
        vout = utxo[0]['vout']

        send_value = satoshi_round((value - fee) / 2)
        inputs = [{'txid': txid, 'vout': vout}]
        outputs = {}
        for i in range(2):
            outputs[self.nodes[0].getnewaddress()] = send_value
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        signedtx = self.nodes[0].signrawtransaction(rawtx)
        txid = self.nodes[0].sendrawtransaction(signedtx['hex'])
        tx0_id = txid
        value = send_value

        # Create tx1
        (tx1_id, tx1_value) = self.chain_transaction(
            self.nodes[0], tx0_id, 0, value, fee, 1)

        # Create tx2-7
        vout = 1
        txid = tx0_id
        for i in range(6):
            (txid, sent_value) = self.chain_transaction(
                self.nodes[0], txid, vout, value, fee, 1)
            vout = 0
            value = sent_value

        # Mine these in a block
        self.nodes[0].generate(1)
        self.sync_all()

        # Now generate tx8, with a big fee
        inputs = [{'txid': tx1_id, 'vout': 0}, {'txid': txid, 'vout': 0}]
        outputs = {self.nodes[0].getnewaddress(): send_value + value - 4 * fee}
        rawtx = self.nodes[0].createrawtransaction(inputs, outputs)
        signedtx = self.nodes[0].signrawtransaction(rawtx)
        txid = self.nodes[0].sendrawtransaction(signedtx['hex'])
        sync_mempools(self.nodes)

        # Now try to disconnect the tip on each node...
        self.nodes[1].invalidateblock(self.nodes[1].getbestblockhash())
        self.nodes[0].invalidateblock(self.nodes[0].getbestblockhash())
        sync_blocks(self.nodes)


if __name__ == '__main__':
    MempoolPackagesTest().main()
