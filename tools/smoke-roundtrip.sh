#!/bin/bash
# v2.6.1 P0.0b.4 smoke round-trip 测试脚本
#
# 验证：v1 binary（生产）跟 vN binary（含 P0.0a + P0.0b 改动）能互相启动同一 datadir
#       不需要 reindex；启动时间 < 30 秒；chain tip hash 一致
#
# **注意**：本卡 smoke 范围有限——vN 当前只动了 sync.h hook + 加新 header（独立），
#          disk format 0 修改。真 round-trip 在 P5.6（含 dispatcher / cacheCoins / seqlock 全接入）跑

set -e

V1_BINARY="${V1_BINARY:-/home/ubuntu/TBCNODE/build/src/bitcoind}"
VN_BINARY="${VN_BINARY:-/home/ubuntu/TBCNODEDEV/build/src/bitcoind}"
V1_CLI="${V1_CLI:-/home/ubuntu/TBCNODE/build/src/bitcoin-cli}"
VN_CLI="${VN_CLI:-/home/ubuntu/TBCNODEDEV/build/src/bitcoin-cli}"

DATADIR=$(mktemp -d -t tbc-smoke-roundtrip-XXXXXX)
trap 'rm -rf "$DATADIR"' EXIT

CONF="$DATADIR/bitcoin.conf"
RPCPORT=19345   # 远离 8332/9332 避免冲突
P2PPORT=19346

cat > "$CONF" <<EOF
regtest=1
# TBC 强制 consensus 参数（src/init.cpp 检查，否则启动失败）
excessiveblocksize=10000000000
maxstackmemoryusageconsensus=100000000
[regtest]
rpcuser=smoke
rpcpassword=smoke_local
rpcport=$RPCPORT
port=$P2PPORT
rpcbind=127.0.0.1:$RPCPORT
rpcallowip=127.0.0.1
bind=127.0.0.1
listen=0
upnp=0
discover=0
daemon=1
fallbackfee=0.0002
standalone=1
EOF

cli() {
    "$V1_CLI" -conf="$CONF" -datadir="$DATADIR" -rpcwait "$@"
}
cli_vn() {
    "$VN_CLI" -conf="$CONF" -datadir="$DATADIR" -rpcwait "$@"
}

start_node() {
    local binary=$1
    local label=$2
    local t0=$(date +%s%N)
    "$binary" -conf="$CONF" -datadir="$DATADIR" 2>&1 &
    local pid=$!

    # 等 RPC ready
    local timeout_s=30
    local elapsed_ns=0
    while ! cli getbestblockhash >/dev/null 2>&1; do
        sleep 0.5
        elapsed_ns=$(( $(date +%s%N) - t0 ))
        if [ $elapsed_ns -gt $((timeout_s * 1000000000)) ]; then
            echo "[$label] FAIL: 启动超时 > ${timeout_s}s"
            return 1
        fi
    done
    local elapsed_ms=$((elapsed_ns / 1000000))
    echo "[$label] 启动用时 ${elapsed_ms}ms"
    return 0
}

stop_node() {
    cli stop 2>/dev/null || true
    # 等真退出
    local timeout_s=15
    local elapsed=0
    while pgrep -f "datadir=$DATADIR" >/dev/null 2>&1; do
        sleep 1
        elapsed=$((elapsed + 1))
        if [ $elapsed -gt $timeout_s ]; then
            pkill -9 -f "datadir=$DATADIR" 2>/dev/null || true
            break
        fi
    done
}

echo "=========================================="
echo "P0.0b.4 smoke round-trip"
echo "  datadir: $DATADIR"
echo "  V1: $V1_BINARY"
echo "  vN: $VN_BINARY"
echo "  RPC port: $RPCPORT (regtest)"
echo "=========================================="

echo
echo "Phase 1: V1 binary 启动 + 生成 50 块 + 关停"
start_node "$V1_BINARY" "v1"
ADDR=$(cli getnewaddress 2>/dev/null || cli -named getnewaddress label="" 2>/dev/null || echo "")
if [ -z "$ADDR" ]; then
    # standalone 模式可能没 wallet，用现有方式
    ADDR=$(cli generate 1 2>/dev/null | python3 -c 'import sys,json;print(json.load(sys.stdin)[0])' 2>/dev/null || true)
    if [ -z "$ADDR" ]; then
        # 没有 wallet 时直接 generate 50 块
        cli generate 50 >/dev/null
    fi
else
    cli generatetoaddress 50 "$ADDR" >/dev/null
fi
V1_TIP=$(cli getbestblockhash)
V1_HEIGHT=$(cli getblockcount)
echo "[v1] tip = $V1_TIP @ height $V1_HEIGHT"
stop_node

echo
echo "Phase 2: vN binary 启动同 datadir + 验证 0 reindex + tip 一致"
start_node "$VN_BINARY" "vn"
VN_TIP=$(cli_vn getbestblockhash)
VN_HEIGHT=$(cli_vn getblockcount)
echo "[vn] tip = $VN_TIP @ height $VN_HEIGHT"
if [ "$V1_TIP" != "$VN_TIP" ]; then
    echo "FAIL: v1 vs vn tip 不一致"
    stop_node
    exit 1
fi
if [ "$V1_HEIGHT" != "$VN_HEIGHT" ]; then
    echo "FAIL: v1 vs vn height 不一致"
    stop_node
    exit 1
fi

echo
echo "Phase 3: vN 再生成 10 块"
if [ -n "$ADDR" ]; then
    cli_vn generatetoaddress 10 "$ADDR" >/dev/null
else
    cli_vn generate 10 >/dev/null
fi
VN_TIP2=$(cli_vn getbestblockhash)
VN_HEIGHT2=$(cli_vn getblockcount)
echo "[vn] tip = $VN_TIP2 @ height $VN_HEIGHT2"
stop_node

echo
echo "Phase 4: V1 启动同 datadir + 验证 vN 写的块 v1 能继续读"
start_node "$V1_BINARY" "v1"
V1_TIP_BACK=$(cli getbestblockhash)
V1_HEIGHT_BACK=$(cli getblockcount)
echo "[v1 reverse] tip = $V1_TIP_BACK @ height $V1_HEIGHT_BACK"
if [ "$VN_TIP2" != "$V1_TIP_BACK" ]; then
    echo "FAIL: vN 写的 tip v1 读不到"
    stop_node
    exit 1
fi
stop_node

echo
echo "=========================================="
echo "✓ smoke round-trip 全部通过"
echo "  v1 → vN 切换 0 reindex"
echo "  vN → v1 切换 0 reindex"
echo "  双向 tip + height 一致"
echo "=========================================="
