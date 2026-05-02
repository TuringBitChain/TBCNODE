#!/usr/bin/env python3
# Copyright (c) 2025 Bitcoin Association
# Distributed under the Open TBC software license, see the accompanying file LICENSE.
"""
v2.6.1 P4 — regtest reorg 注入 functional test (v4 §2.4)

目的：
  Phase 2/3 期 mainnet 自然 reorg 概率低，靠 regtest 主动注入：
    1. 启动 2 个 regtest 节点 A / B（一开始连通，初始化共同链）
    2. 断开 → A 挖 10 块、B 挖 11 块（B 的链更长，将获胜）
    3. 期间在两边 mempool 注入 tx (best-effort — 见 ENABLE_MEMPOOL_INJECT)
    4. 重新连通 → 触发 reorg
    5. reorg 完成后 sync_all，断言 (tip, UTXO, mempool) 在两节点间一致

  覆盖 v6 §3 / 设计文档 P4.1 ConnectTip / Phase 3 sync 模式
  的 (tip, UTXO, mempool) 同 epoch 不变量。

通过标准：
  reorg 完成后两节点
    - 同一 best block hash
    - 同一 UTXO set hash (gettxoutsetinfo.hash_serialized)
    - 同一 mempool size (再 sync_mempools 后)
  无 hang / crash。

环境注意：
  当前 dev 分支 wallet `sendtoaddress` 走 `g_dispatcher.SubmitSync`，
  存在已知 30s/tx 等待问题（worker 30s 才完成）。`ENABLE_MEMPOOL_INJECT=False`
  时 reorg 测试跳过 mempool 注入，仅验证空 mempool reorg —— 仍能 exercise
  `ConnectTip` / Phase 3 `RemoveForBlockNL` 路径，但覆盖度较低。
  环境变量 `TBC_REORG_INJECT_MEMPOOL=1` 可强制开启注入。

兼容性：
  cmake --build build --target check-functional 会自动 pick up
  （test_runner.py 用 get_all_scripts_from_disk）。
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
    connect_nodes_bi,
    disconnect_nodes_bi,
    sync_blocks,
    sync_mempools,
    wait_until,
)
from decimal import Decimal
import os


REORG_ITERATIONS = 2   # 跑 N 轮 reorg；regtest 上每轮 ~30s（无 mempool 注入）

# dev 分支 sendtoaddress 当前有 30s/tx 等待 bug；默认关闭注入。
# 设置 TBC_REORG_INJECT_MEMPOOL=1 强制开启。
ENABLE_MEMPOOL_INJECT = bool(os.environ.get("TBC_REORG_INJECT_MEMPOOL"))
INJECT_TXS_PER_ITER = 4   # 每轮注入多少笔（很少，避免 30s × N 总耗时）
INJECT_TIMEOUT_S = 60     # 单笔 sendtoaddress 超过这个时长就放弃


class V261ReorgInjectionTest(BitcoinTestFramework):

    def set_test_params(self):
        self.num_nodes = 2
        self.setup_clean_chain = True
        # -whitelist=127.0.0.1 让对端 tx 不被 ratelimit
        # -removeforblock=sync 走 Phase 3 sync 路径（默认值，显式写出来）
        self.extra_args = [
            ["-whitelist=127.0.0.1",
             "-removeforblock=sync",
             "-debug=mempool",
             "-debug=bench"],
            ["-whitelist=127.0.0.1",
             "-removeforblock=sync",
             "-debug=mempool",
             "-debug=bench"],
        ]

    # ---------- helpers ----------

    def get_chain_state(self, node):
        """采集 (tip, UTXO root, mempool size)。"""
        tip = node.getbestblockhash()
        utxo_info = node.gettxoutsetinfo()
        mempool = node.getrawmempool()
        return {
            "tip": tip,
            "height": utxo_info["height"],
            "utxo_hash": utxo_info["hash_serialized"],
            "utxo_count": utxo_info["txouts"],
            "mempool_size": len(mempool),
            "mempool_set": set(mempool),
        }

    def assert_consistent(self, label):
        s0 = self.get_chain_state(self.nodes[0])
        s1 = self.get_chain_state(self.nodes[1])
        self.log.info(
            "[{}] node0: tip={}.. h={} utxo={} mp={} | "
            "node1: tip={}.. h={} utxo={} mp={}".format(
                label,
                s0["tip"][:12], s0["height"], s0["utxo_count"], s0["mempool_size"],
                s1["tip"][:12], s1["height"], s1["utxo_count"], s1["mempool_size"]))
        assert_equal(s0["tip"], s1["tip"])
        assert_equal(s0["height"], s1["height"])
        assert_equal(s0["utxo_hash"], s1["utxo_hash"])
        assert_equal(s0["utxo_count"], s1["utxo_count"])
        assert_equal(s0["mempool_size"], s1["mempool_size"])
        assert_equal(s0["mempool_set"], s1["mempool_set"])
        return s0

    def inject_mempool_tx(self, node, count, label):
        """
        往 node 的 mempool 注入 count 笔 self-send tx。返回成功的 txid 列表。
        若环境关闭注入（ENABLE_MEMPOOL_INJECT=False）直接 skip。
        """
        if not ENABLE_MEMPOOL_INJECT:
            self.log.info(
                "[{}] mempool injection skipped (set TBC_REORG_INJECT_MEMPOOL=1 "
                "to enable; dev branch sendtoaddress 当前有 30s/tx 已知问题)".format(label))
            return []

        import time as _time
        addr = node.getnewaddress()
        sent = []
        for _ in range(count):
            t0 = _time.time()
            try:
                txid = node.sendtoaddress(addr, Decimal("0.001"))
                sent.append(txid)
                if _time.time() - t0 > INJECT_TIMEOUT_S:
                    self.log.info("[{}] sendtoaddress slow ({:.1f}s); abort further inject".format(
                        label, _time.time() - t0))
                    break
            except Exception as e:
                self.log.info("[{}] sendtoaddress aborted: {}".format(label, e))
                break
        self.log.info("[{}] injected {} tx into {}'s mempool".format(
            label, len(sent), node.url))
        return sent

    # ---------- main ----------

    def run_test(self):
        node_a = self.nodes[0]
        node_b = self.nodes[1]

        # 1) 共同链 + 给两边足够的 spendable utxo
        self.log.info("Phase 0: bootstrap common chain")
        # Node A 挖 200 块（100 成熟 + spendable），传给 B
        node_a.generate(200)
        self.sync_all()
        # 让 B 也挖一些以拥有自己的 spendable
        node_b.generate(20)
        self.sync_all()
        # 起跑前一致性检查
        self.assert_consistent("bootstrap")

        for it in range(1, REORG_ITERATIONS + 1):
            self.log.info("==== reorg iter {}/{} ====".format(it, REORG_ITERATIONS))

            # 2) 注入 mempool tx（连通状态下，确保两边都有共同 mempool）
            shared_txs = self.inject_mempool_tx(node_a, INJECT_TXS_PER_ITER, "iter{}-shared".format(it))
            if shared_txs:
                sync_mempools(self.nodes, timeout=120)
                assert_equal(len(node_a.getrawmempool()),
                             len(node_b.getrawmempool()))
            shared_mp_size = len(node_a.getrawmempool())
            self.log.info("[iter{}] shared mempool size before split: {}".format(
                it, shared_mp_size))

            # 3) 断开 → 双链 fork
            disconnect_nodes_bi(self.nodes, 0, 1)

            # A 挖 10 块（短链），把部分 tx 打进块（mempool 这一侧会清掉）
            tip_before = node_a.getbestblockhash()
            blocks_a = node_a.generate(10)
            self.log.info("[iter{}] node A mined 10 blocks; tip {}.. -> {}..".format(
                it, tip_before[:12], blocks_a[-1][:12]))

            # B 挖 11 块（长链）
            tip_before_b = node_b.getbestblockhash()
            blocks_b = node_b.generate(11)
            self.log.info("[iter{}] node B mined 11 blocks; tip {}.. -> {}..".format(
                it, tip_before_b[:12], blocks_b[-1][:12]))

            # 4) 期间分别注入 mempool（fork 期间各自 mempool 独立）
            extra_a = self.inject_mempool_tx(node_a, INJECT_TXS_PER_ITER, "iter{}-A-fork".format(it))
            extra_b = self.inject_mempool_tx(node_b, INJECT_TXS_PER_ITER, "iter{}-B-fork".format(it))

            self.log.info("[iter{}] pre-reconnect: A mp={} B mp={}".format(
                it, len(node_a.getrawmempool()), len(node_b.getrawmempool())))

            # 5) 重连 → 触发 reorg：A 必须丢掉自己 10 块、采纳 B 的 11 块
            connect_nodes_bi(self.nodes, 0, 1)

            # sync_blocks 内部 wait_until tip 一致
            sync_blocks(self.nodes, timeout=120)

            # tip 应该是 B 的最后一块（B 链更长，胜出）
            assert_equal(node_a.getbestblockhash(), blocks_b[-1])
            assert_equal(node_b.getbestblockhash(), blocks_b[-1])
            self.log.info("[iter{}] reorg done; common tip = {}..".format(
                it, blocks_b[-1][:12]))

            # 6) reorg 后 mempool 同步（reorg 把 A 的 10 个块的 tx 退回 mempool；
            #    sync 后两边 mempool 该一致）
            try:
                sync_mempools(self.nodes, timeout=60)
            except AssertionError as e:
                # 注入关闭时 mempool 一般为空；reorg 把 A 的 10 块 coinbase
                # 不会进 mempool（coinbase 不能 relay），所以 sync 通常瞬间通过。
                # 如果 sync 失败，记录 mempool 状态再 raise。
                self.log.info("[iter{}] sync_mempools failed: A_mp={} B_mp={}".format(
                    it, len(node_a.getrawmempool()), len(node_b.getrawmempool())))
                raise

            # 7) 一致性断言
            state = self.assert_consistent("iter{}-post-reorg".format(it))
            self.log.info(
                "[iter{}] post-reorg: tip={}.. height={} utxo_hash={}.. mp={}".format(
                    it, state["tip"][:12], state["height"],
                    state["utxo_hash"][:16], state["mempool_size"]))

            # 8) 把 mempool 清干净（多挖几块），为下一轮做准备
            node_a.generate(2)
            self.sync_all()
            self.assert_consistent("iter{}-clean".format(it))

        self.log.info("==== reorg injection PASSED for {} iterations ====".format(
            REORG_ITERATIONS))


if __name__ == "__main__":
    V261ReorgInjectionTest().main()
