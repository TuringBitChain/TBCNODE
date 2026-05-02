#!/bin/bash
# v2.6.1 P0.6 SCAFFOLD: TSan / ASan / helgrind 72h 长压脚本
#
# **本地 dev session 不能跑 72h，留 dedicated CI 机器**。
# 业务方实施时跑：
#   1. 在 dedicated machine 上 build TSan/ASan variant
#   2. 后台启动 72h 循环跑 P0 单元 + functional test
#   3. 收集 race / leak count，跟 P0.0a.2 baseline 对比
#
# KPI（GATE-M0 必过）：
#   - TSan 72h 新增 race vs P0.0a.2 baseline = 0
#   - helgrind 24h 新增 race = 0
#   - ASan 72h 新增 leak = 0

set -e

DURATION_H="${DURATION_H:-72}"
BUILD_TSAN="${BUILD_TSAN:-build-tsan}"
BUILD_ASAN="${BUILD_ASAN:-build-asan}"
LOG_DIR="${LOG_DIR:-/var/log/p0-soak}"
mkdir -p "$LOG_DIR"

cd /home/ubuntu/TBCNODEDEV

echo "=== Step 1: 编译 TSan / ASan variant ==="
cmake -B "$BUILD_TSAN" -S . -DENABLE_PROD_BUILD=ON -DBUILD_BITCOIN_WALLET=OFF -Denable_tsan=ON
cmake --build "$BUILD_TSAN" -j8 --target test_bitcoin

cmake -B "$BUILD_ASAN" -S . -DENABLE_PROD_BUILD=ON -DBUILD_BITCOIN_WALLET=OFF -Denable_asan=ON
cmake --build "$BUILD_ASAN" -j8 --target test_bitcoin

echo "=== Step 2: 启动 72h 循环跑 ==="
export TSAN_OPTIONS="halt_on_error=0:second_deadlock_stack=1:suppressions=tools/sanitizer-suppression.txt"
export ASAN_OPTIONS="halt_on_error=0:detect_leaks=1"

end_ts=$(($(date +%s) + DURATION_H * 3600))
ROUND=0
while [ "$(date +%s)" -lt "$end_ts" ]; do
    ROUND=$((ROUND + 1))
    echo "--- Round $ROUND $(date) ---"

    # 跑 P0 测试套件（重点）
    "./$BUILD_TSAN/src/test/test_bitcoin" \
        --run_test=coins_tests,coins_p02_dual_write,coins_p03_getcoinconcurrent,batchwrite_p99,chainstate_seqlock,libcuckoo_soak,recursive_mutex_spike,lock_hierarchy_tests \
        2>&1 | tee -a "$LOG_DIR/tsan-round-$ROUND.log"

    "./$BUILD_ASAN/src/test/test_bitcoin" \
        --run_test=coins_tests,coins_p02_dual_write,coins_p03_getcoinconcurrent \
        2>&1 | tee -a "$LOG_DIR/asan-round-$ROUND.log"
done

echo "=== Step 3: 汇总 race / leak count ==="
TSAN_RACES=$(grep -c "WARNING: ThreadSanitizer" "$LOG_DIR"/tsan-*.log 2>/dev/null | awk -F: '{s+=$2} END {print s}')
ASAN_LEAKS=$(grep -c "ERROR: LeakSanitizer\|ERROR: AddressSanitizer" "$LOG_DIR"/asan-*.log 2>/dev/null | awk -F: '{s+=$2} END {print s}')

echo "TSan races: $TSAN_RACES"
echo "ASan leaks: $ASAN_LEAKS"
echo
echo "对比 P0.0a.2 baseline（如已跑）："
echo "  baseline TSan races: <P0.0a.2 输出>"
echo "  baseline ASan leaks: <P0.0a.2 输出>"
echo
echo "KPI 通过条件：新增 = 0"
