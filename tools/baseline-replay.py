#!/usr/bin/env python3
"""
v2.6.1 P0.0b.3 共识等价性 baseline 采样窗口脚本（SCAFFOLD）

业务方运维实施时跑（需要独立 datadir 副本 + 4-8 实例并发，3-4 周机器跑完）

策略（详细设计 §9.P0.0b.3 + R-risk-register R4）：
  - 关键激活高度 ±1000 块全量
  - 每 5000 块 ±100 块全量采样
  - 整体覆盖率 > 30%，敏感区段 100%

输出：consensus-baseline-v2.6.1.json
{
  "schema_version": 1,
  "binary_sha256": "...",
  "tbc_first_block_height": 824190,
  "samples": [
    {
      "height": 824190,
      "best_block_hash": "...",
      "utxo_hash_serialized_2": "...",
      "tx_count_in_block": ...,
      "mempool_size_at_capture": ...
    },
    ...
  ]
}

P6.4 用此 baseline 对比 v2.6.1 跑出的同高度 hash，0 偏差才能进 P6.7。
"""

import argparse, json, os, subprocess, sys, time
from pathlib import Path

# TBC chainparams 硬编码（src/chainparams.cpp:107）
TBC_FIRST_BLOCK_HEIGHT = 824190

# 关键激活高度（每个 ±1000 块全量；运维实施前从 chainparams.cpp 重新核对）
ACTIVATION_HEIGHTS = [
    824190,    # TBC Genesis
    # 待运维补：schnorrMultisigHeight, kycV1, kycV2 等（grep chainparams.cpp::*Height）
]


def sampling_windows(tbc_first: int, tbc_tip: int) -> list[int]:
    """生成采样高度集合"""
    heights = set()
    # 关键激活高度 ±1000 块全量
    for h in ACTIVATION_HEIGHTS:
        for d in range(-1000, 1001):
            target = h + d
            if tbc_first <= target <= tbc_tip:
                heights.add(target)
    # 每 5000 块 ±100 块全量
    for h in range(tbc_first, tbc_tip + 1, 5000):
        for d in range(-100, 101):
            target = h + d
            if tbc_first <= target <= tbc_tip:
                heights.add(target)
    return sorted(heights)


def rpc(datadir: str, rpcport: int, method: str, *args) -> dict:
    """通过 bitcoin-cli 调 RPC"""
    cmd = ['bitcoin-cli', f'-datadir={datadir}', f'-rpcport={rpcport}',
           '-rpcuser=baseline', '-rpcpassword=baseline_local',
           method] + [str(a) for a in args]
    try:
        out = subprocess.check_output(cmd, timeout=120)
        return json.loads(out) if out.strip().startswith(b'{') else out.decode().strip()
    except subprocess.TimeoutExpired:
        return {'error': 'rpc-timeout'}
    except subprocess.CalledProcessError as e:
        return {'error': str(e)}


def replay_at_height(datadir: str, rpcport: int, target_height: int) -> dict:
    """invalidateblock + reconsiderblock 让节点 active chain 停在 target_height"""
    # 简化版：实际运维实施时用 invalidateblock 链回滚到 target+1，让 active tip = target
    # （不是 reindex，每次只回滚 1 块，速度快）
    best = rpc(datadir, rpcport, 'getbestblockhash')
    cur_height = rpc(datadir, rpcport, 'getblockcount')
    # 回滚到 target_height（如有需要）
    while cur_height > target_height:
        block_hash = rpc(datadir, rpcport, 'getblockhash', cur_height)
        rpc(datadir, rpcport, 'invalidateblock', block_hash)
        cur_height = rpc(datadir, rpcport, 'getblockcount')

    # 拍 utxo set hash
    utxoinfo = rpc(datadir, rpcport, 'gettxoutsetinfo')
    if isinstance(utxoinfo, dict) and 'hash_serialized_2' in utxoinfo:
        return {
            'height': target_height,
            'best_block_hash': rpc(datadir, rpcport, 'getbestblockhash'),
            'utxo_hash_serialized_2': utxoinfo['hash_serialized_2'],
            'tx_count_in_block': utxoinfo.get('transactions', 0),
        }
    return {'height': target_height, 'error': 'gettxoutsetinfo failed'}


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--datadir', required=True, help='独立 datadir 副本（不能用生产）')
    p.add_argument('--rpcport', type=int, default=9332)
    p.add_argument('--instance', type=int, default=0, help='实例 id（0..total-1）')
    p.add_argument('--total', type=int, default=8, help='总实例数')
    p.add_argument('--tbc-tip', type=int, required=True, help='当前 TBC mainnet 链高')
    p.add_argument('--output', required=True)
    args = p.parse_args()

    if 'bitcoin' in args.datadir and 'tbcnodedev' not in args.datadir:
        sys.exit('ERROR: 拒绝在生产 datadir 上跑 invalidateblock。请用独立副本。')

    heights = sampling_windows(TBC_FIRST_BLOCK_HEIGHT, args.tbc_tip)
    # 切片：本实例处理 heights[instance::total]
    my_heights = heights[args.instance::args.total]
    print(f'[instance {args.instance}/{args.total}] 处理 {len(my_heights)} 个高度', flush=True)

    samples = []
    t0 = time.time()
    for i, h in enumerate(my_heights):
        sample = replay_at_height(args.datadir, args.rpcport, h)
        samples.append(sample)
        if (i + 1) % 10 == 0:
            elapsed = time.time() - t0
            rate = (i + 1) / elapsed
            eta = (len(my_heights) - i - 1) / rate if rate > 0 else 0
            print(f'  进度 {i+1}/{len(my_heights)}, ETA {eta/3600:.1f}h', flush=True)

    out = {
        'schema_version': 1,
        'instance': args.instance,
        'total': args.total,
        'tbc_first_block_height': TBC_FIRST_BLOCK_HEIGHT,
        'tbc_tip': args.tbc_tip,
        'sample_count': len(samples),
        'samples': samples,
    }
    Path(args.output).write_text(json.dumps(out, indent=2))
    print(f'写入 {args.output}', flush=True)


if __name__ == '__main__':
    main()
