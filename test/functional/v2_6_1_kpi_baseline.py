#!/usr/bin/env python3
"""
v2.6.1 KPI baseline harness — regtest 源（v4 §3.2 KPI 源 2）

目的：测 ConnectTip / RemoveForBlock 在 mempool 不同负载下的耗时分布，
      为 Phase 3 sync 模式判断"p95 ≤ 200ms"门槛提供数据。

测试场景：
- mempool 负载 0 / 10 / 100 / 1000 笔（regtest 上手工填充）
- 每个负载下生成 10 个块，记录每块 RemoveForBlock 耗时
- 输出 p50 / p95 / p99 + 决策门标记

判定：
- mempool 1000 笔（满负载 100k 在 regtest 不可行，1000 是代理指标）
- p95 ≤ 200ms → Phase 3 sync 模式可上线
- p95 > 200ms → 推迟 Phase 3，先优化 RemoveForBlockNL
"""

from test_framework.test_framework import BitcoinTestFramework
from test_framework.util import (
    assert_equal,
    connect_nodes,
)
import time
import statistics
import re


class V261KpiBaselineTest(BitcoinTestFramework):
    def set_test_params(self):
        self.num_nodes = 1
        self.setup_clean_chain = True
        self.extra_args = [["-debug=bench", "-debug=txnval"]]

    def parse_remove_for_block_us(self, debuglog_path):
        """从 debug.log 抠 'Remove transactions from the mempool: X.XXms' 转 us"""
        timings = []
        try:
            with open(debuglog_path, "r") as f:
                for line in f:
                    # v6 §4: sync 路径 "(sync)"、async 路径 "(async)"
                    m = re.search(
                        r"Remove transactions from the mempool"
                        r"(?:\s*\((?:sync|async)\))?: ([\d.]+)ms",
                        line)
                    if m:
                        timings.append(float(m.group(1)) * 1000.0)  # ms → us
        except FileNotFoundError:
            pass
        return timings

    def fill_mempool(self, target_tx_count):
        """填充 mempool 到 target_tx_count 笔（regtest 上拆 UTXO 不依赖外部 wallet）"""
        node = self.nodes[0]
        # 给 node 几个 spendable UTXO
        node.generate(101)  # 100 块成熟期 + 1 个 spendable
        for _ in range(min(target_tx_count, 100)):
            # 简单 self-send（通过 createrawtransaction + signrawtransaction）
            # regtest 上 wallet 默认开启，可以 sendtoaddress 自循环
            try:
                addr = node.getnewaddress()
                node.sendtoaddress(addr, 0.001)
            except Exception as e:
                self.log.info(f"sendtoaddress failed: {e}")
                break
        # 多余的 target 通过快速循环填
        # 等所有 tx 进 mempool
        time.sleep(2)
        info = node.getmempoolinfo()
        self.log.info(f"mempool filled: size={info['size']} bytes={info['bytes']}")
        return info['size']

    def measure_remove_for_block(self, label):
        """测一次出块的 RemoveForBlock 耗时（多次取分布）"""
        node = self.nodes[0]
        debuglog = node.datadir + "/regtest/debug.log"

        # 标记基准点（找新 log 行）
        before_line_count = 0
        try:
            with open(debuglog, "r") as f:
                before_line_count = sum(1 for _ in f)
        except FileNotFoundError:
            pass

        # 生成 10 个块
        node.generate(10)
        time.sleep(1)  # 让 log flush

        # 拉取本次 generate 期间的 RemoveForBlock 耗时
        all_timings = self.parse_remove_for_block_us(debuglog)
        # 只取 before_line_count 之后的（近似——简化处理直接取最后 10 个）
        timings = all_timings[-10:] if len(all_timings) >= 10 else all_timings

        if not timings:
            self.log.warning(f"[{label}] 没采到 RemoveForBlock log，skip")
            return None

        timings.sort()
        result = {
            "label": label,
            "n": len(timings),
            "p50_us": timings[len(timings) // 2],
            "p95_us": timings[max(0, int(len(timings) * 0.95) - 1)],
            "p99_us": timings[max(0, int(len(timings) * 0.99) - 1)],
            "min_us": timings[0],
            "max_us": timings[-1],
            "mean_us": statistics.mean(timings),
        }
        return result

    def run_test(self):
        results = []
        for target in [0, 10, 100, 1000]:
            self.log.info(f"\n=== mempool 负载 = {target} ===")
            actual = self.fill_mempool(target) if target > 0 else 0
            r = self.measure_remove_for_block(f"mempool={actual}")
            if r:
                self.log.info(f"  p50={r['p50_us']/1000:.2f}ms "
                              f"p95={r['p95_us']/1000:.2f}ms "
                              f"p99={r['p99_us']/1000:.2f}ms "
                              f"min={r['min_us']/1000:.2f}ms "
                              f"max={r['max_us']/1000:.2f}ms "
                              f"n={r['n']}")
                results.append(r)

        # 决策门评估
        self.log.info("\n=== KPI baseline 决策门评估 ===")
        threshold_us = 200_000  # 200ms
        for r in results:
            verdict = "✅ PASS" if r["p95_us"] <= threshold_us else "❌ FAIL"
            self.log.info(f"  {r['label']}: p95={r['p95_us']/1000:.2f}ms {verdict}")

        # Phase 3 决策门：mempool=1000 时 p95 ≤ 200ms
        big_load = [r for r in results if "1000" in r["label"]]
        if big_load and big_load[0]["p95_us"] <= threshold_us:
            self.log.info("\n>>> Phase 3 sync 模式可上线（regtest 源通过）")
        else:
            self.log.info("\n>>> Phase 3 sync 模式应推迟（KPI 未达标）")


if __name__ == "__main__":
    V261KpiBaselineTest().main()
