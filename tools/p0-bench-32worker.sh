#!/bin/bash
# v2.6.1 P0.7 SCAFFOLD: 32 worker 16x bench 脚本
#
# **本地 dev session 不跑（需 dedicated 32+ 核机器），留 GATE-M0 实施跑**。
#
# KPI（GATE-M0 必过）：
#   - 32 worker 并发 GetCoinConcurrent 吞吐 ≥ 单线程 16x（无 BatchWrite 窗口）
#   - 含 BatchWrite 窗口（10 min 周期）≥ 8x
#   - BatchWrite p99（10 万 UTXO）≤ 200ms
#   - 单线程吞吐 vs P0 前不退化（≥ 95%）

set -e

cd /home/ubuntu/TBCNODEDEV

echo "=== Step 1: 编译 release variant ==="
cmake -B build -S . -DENABLE_PROD_BUILD=ON -DBUILD_BITCOIN_WALLET=OFF
cmake --build build -j$(nproc) --target test_bitcoin

echo "=== Step 2: 跑 32 worker 并发吞吐 bench ==="
# 当前已有 P0.3 单元测试 concurrent_32_workers_no_race（2 秒 sanity）
# GATE-M0 需要更大规模 bench：5000 万 UTXO baseline + 32 worker × 1h
# 用 src/bench/ 目标（待加 coins_bench.cpp）

# 当前 sanity（基线，不是真 KPI）
./build/src/test/test_bitcoin --run_test=coins_p03_getcoinconcurrent/concurrent_32_workers_no_race --log_level=test_suite 2>&1 | tail -5

echo
echo "=== Step 3: 真 KPI bench 待加 src/bench/coins_concurrent_bench.cpp ==="
# 框架（GATE-M0 实施时写）：
#   BENCHMARK(GetCoinConcurrent_32worker)->Threads(32)->UseRealTime();
#   BENCHMARK(GetCoinConcurrent_1worker)->Threads(1);
#   BENCHMARK(Mixed_GetCoin_BatchWrite_32worker)->Threads(32);
# 5000 万 UTXO baseline 预填，跑 1-2 小时，输出 ops/s
# 对比单线程 baseline，验证 ≥ 16x

echo "GATE-M0 KPI（待 dedicated 机器跑）："
echo "  - 32 worker 吞吐 ≥ 单线程 16x"
echo "  - 含 BatchWrite 窗口 ≥ 8x"
echo "  - BatchWrite p99 ≤ 200ms"
echo "  - 单线程不退化 ≥ 95%"
