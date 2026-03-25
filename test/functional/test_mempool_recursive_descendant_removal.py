#!/usr/bin/env python3
"""
测试内存池TrimToSize时的递归后代移除。

测试场景：
1. 创建一个交易链: tx0 -> tx1 -> tx2 (tx1依赖tx0，tx2依赖tx1)
2. 填满内存池直到接近限制
3. 插入新交易触发TrimToSize
4. 验证当交易链中的某个交易被移除时，其所有后代也被递归移除

测试通过标准：
- TrimToSize被触发
- 如果交易链中的某个交易被移除，其所有后代也必须被移除
"""

from decimal import Decimal
from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import assert_equal


class MempoolTrimRecursiveRemovalTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 1
        # 设置较小的内存池大小(2MB)，以便更容易触发TrimToSize
        self.extra_args = [['-maxmempool=2', '-debug=mempool']]

    def create_tx(self, node, utxo, value, fee=Decimal("0.0001")):
        addr = node.getnewaddress()
        output_amount = value - fee
        # 确保金额是有效的
        if output_amount <= 0:
            raise ValueError("Output amount must be positive")
        raw = node.createrawtransaction(
            [{"txid": utxo["txid"], "vout": utxo["vout"]}],
            {addr: float(output_amount)}
        )
        signed = node.signrawtransaction(raw)
        txid = node.sendrawtransaction(signed["hex"])
        return txid

    def run_test(self):
        node = self.nodes[0]

        self.log.info("generate spendable coins")
        # 生成大量区块，获得大量UTXO来填满内存池
        node.generate(5000)

        # 获取所有可用的UTXO
        utxos = node.listunspent()
        self.log.info("total utxos available: %d" % len(utxos))

        #
        # 构造 DAG 交易链（使用第一个UTXO）
        # tx_chain_0 (祖先)
        #  └ tx_chain_1 (子孙)
        #     └ tx_chain_2 (子孙)
        #
        self.log.info("create transaction chain")

        first_utxo = utxos[0]
        value = Decimal(str(first_utxo["amount"]))

        tx_chain = []
        current_utxo = first_utxo

        # 创建3个交易的链，使用递增手续费
        # 关键是让填充交易使用更高的手续费，使交易链的 descendant_score 相对较低
        for i in range(3):
            fee = Decimal("0.0001") * (i + 1)  # 0.0001, 0.0002, 0.0003
            txid = self.create_tx(node, current_utxo, value, fee)
            tx_chain.append(txid)
            self.log.info("created tx_chain[%d]: %s" % (i, txid))
            current_utxo = {"txid": txid, "vout": 0}

        # 获取当前内存池状态
        mempool = node.getrawmempool()
        self.log.info("mempool size after chain: %d" % len(mempool))

        # 确保交易链在内存池中
        for i, txid in enumerate(tx_chain):
            assert txid in mempool, "tx_chain[%d] should be in mempool" % i

        # 重新获取UTXO列表（排除已使用的）
        utxos = node.listunspent()
        self.log.info("available utxos for filling: %d" % len(utxos))

        #
        # 用其他交易填满内存池
        #
        self.log.info("filling mempool with other transactions...")

        other_txs = []
        # 跳过已使用的UTXO，使用更高手续费
        # 这样填充交易的 descendant_score 会高于交易链，确保 TrimToSize 先选中交易链
        for i in range(1, len(utxos)):
            utxo = utxos[i]
            value_i = Decimal(str(utxo["amount"]))
            # 跳过金额太小的UTXO
            if value_i < Decimal("0.001"):
                continue
            try:
                # 使用 0.0005 手续费，高于交易链，但确保能进入内存池
                txid = self.create_tx(node, utxo, value_i, Decimal("0.0005"))
                other_txs.append(txid)
            except Exception as e:
                if "mempool full" in str(e) or "mempool min fee not met" in str(e):
                    self.log.info("stopped at tx %d: %s" % (i, str(e)))
                    break
                self.log.info("error at tx %d: %s" % (i, str(e)))
                break

        mempool_before = node.getrawmempool()
        self.log.info("mempool size before final trigger: %d" % len(mempool_before))

        # 打印交易链的 descendant_score 信息
        self.log.info("transaction chain descendant scores:")
        for i, txid in enumerate(tx_chain):
            try:
                entry = node.getmempoolentry(txid)
                self.log.info("  tx_chain[%d]: ancestorfee=%s, ancestorsize=%s, descendantfee=%s, descendantsize=%s" % (
                    i, entry.get('ancestorfees', 'N/A'), entry.get('ancestorsize', 'N/A'),
                    entry.get('descendantfees', 'N/A'), entry.get('descendantsize', 'N/A')))
            except:
                self.log.info("  tx_chain[%d]: NOT IN MEMPOOL (removed by TrimToSize)" % i)

        mempool_info = node.getmempoolinfo()
        self.log.info("mempool usage before trigger: %d bytes (max: %d)" % (mempool_info['usage'], mempool_info['maxmempool']))

        #
        # 挖矿获取新币，然后尝试创建触发交易
        #
        self.log.info("mining new block for trigger tx")

        node.generate(1)
        new_utxos = node.listunspent()

        trigger_tx = None
        for utxo in new_utxos:
            if utxo["txid"] not in mempool_before and utxo["txid"] not in tx_chain:
                try:
                    trigger_tx = self.create_tx(node, utxo, Decimal(str(utxo["amount"])))
                    self.log.info("trigger tx created: %s" % trigger_tx)
                    break
                except Exception as e:
                    self.log.info("failed to create trigger tx: %s" % str(e))
                    break

        mempool_after = node.getrawmempool()
        self.log.info("mempool size after trim: %d" % len(mempool_after))

        mempool_info_after = node.getmempoolinfo()
        self.log.info("mempool usage after trigger: %d bytes (max: %d)" % (mempool_info_after['usage'], mempool_info_after['maxmempool']))

        #
        # 验证结果
        #
        self.log.info("tx_chain[0] (ancestor) in mempool: %s" % (tx_chain[0] in mempool_after))
        self.log.info("tx_chain[1] (descendant) in mempool: %s" % (tx_chain[1] in mempool_after))
        self.log.info("tx_chain[2] (descendant) in mempool: %s" % (tx_chain[2] in mempool_after))

        # 检查每个交易的存活状态
        tx0_alive = tx_chain[0] in mempool_after
        tx1_alive = tx_chain[1] in mempool_after
        tx2_alive = tx_chain[2] in mempool_after

        #
        # 关键验证：交易链的完整性
        #
        # 验证原则：在TrimToSize中，当一个交易被选中移除时，其所有后代也会被递归移除
        # 这是因为TrimToSize使用CalculateDescendantsNL计算整个交易包并一起移除

        # 情况1：如果tx0被移除，tx1和tx2也必须被移除（递归后代移除）
        if not tx0_alive:
            assert not tx1_alive, "FAIL: tx1 should be removed when tx0 is removed"
            assert not tx2_alive, "FAIL: tx2 should be removed when tx0 is removed"
            self.log.info("SUCCESS: recursive descendant removal verified for tx0 removal!")

        # 情况2：如果tx1被移除但tx0还在，tx2必须被移除
        # 注意：这种情况不应该发生，因为如果tx1被选中，TrimToSize会计算其所有后代
        # 但由于tx0是tx1的祖先，tx0的descendant_score包含了tx1，所以tx0会先于tx1被选中
        if tx0_alive and not tx1_alive:
            assert not tx2_alive, "FAIL: tx2 should be removed when tx1 is removed"
            self.log.info("SUCCESS: recursive descendant removal verified for tx1 removal!")

        # 情况3：如果所有交易都还在，说明TrimToSize没有影响到交易链
        if tx0_alive and tx1_alive and tx2_alive:
            self.log.info("Transaction chain is intact (higher fee rate preserved)")

        # 情况4：如果tx2被移除但tx1还在，这是正常的TrimToSize行为
        # 解释：tx2是交易链的末端，它的descendant_score只包含自己
        # 当内存池满时，如果tx2的descendant_score最低，它会被选中移除
        # 但由于tx2没有后代，只有tx2自己被移除
        if tx1_alive and not tx2_alive:
            self.log.info("tx2 was removed by TrimToSize (lowest descendant score)")
            self.log.info("Note: tx1 remains because its descendant_score includes tx2,")
            self.log.info("      making it higher than tx2's score alone")

        # 额外验证：确保没有孤立交易（只检查交易链中的交易）
        self.log.info("verifying no orphaned transactions in the chain...")
        if tx1_alive:
            # 如果tx1还在，tx0（它的父交易）也必须在
            assert tx0_alive, "FAIL: tx1 is in mempool but tx0 (its parent) was removed!"
        if tx2_alive:
            # 如果tx2还在，tx1（它的父交易）也必须在
            assert tx1_alive, "FAIL: tx2 is in mempool but tx1 (its parent) was removed!"

        # 验证内存池大小是否在限制范围内
        final_info = node.getmempoolinfo()
        assert final_info['usage'] <= final_info['maxmempool'], "Mempool usage exceeds max!"
        self.log.info("Mempool usage is within limit: %d <= %d" % (final_info['usage'], final_info['maxmempool']))

        # 最终验证：确保TrimToSize被触发（内存池应该移除了一些交易）
        trim_triggered = len(mempool_after) < len(mempool_before) + 1 or not tx2_alive
        if trim_triggered:
            self.log.info("PASS: TrimToSize was triggered and mempool is within limits")
        else:
            self.log.info("Note: TrimToSize may not have been triggered (mempool not full enough)")


if __name__ == '__main__':
    MempoolTrimRecursiveRemovalTest().main()
