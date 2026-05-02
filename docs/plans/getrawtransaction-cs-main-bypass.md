# getrawtransaction 取 raw 数据的 cs_main 旁路改造

**目标读者**：核心 C++ 开发 / 评审者
**改动范围**：`src/rpc/rawtransaction.cpp`、`src/validation.{h,cpp}`、`src/txmempool.cpp`
**性质**：仅在原 `getrawtransaction` 内部按 `verbose` 分流读路径，不新增对外接口、不触碰共识、不改变任何已有 RPC 在原参数下的对外语义

---

## 0. 一句话总结

> 把"只取 raw bytes"的事务读路径从 `cs_main` 大锁中**完全摘出**，单独走 `mempool.smtx` 共享读锁 + LevelDB（`pblocktree`）+ 追加写的块文件。这是当前生产环境**无法回避**的一类 RPC，必须解掉它持锁带来的全局节流。

---

## 1. 背景：生产环境为什么"取 raw 数据"不可避免

TBC 主网上有一类高频客户端，它们的核心动作是**已知 txid → 拿 raw bytes**，这是节点对外**必查**的一类调用，典型场景包括但不限于：

- **索引**：FT / NFT 索引器、Pool / OrderBook 跟踪器等服务，合约语义全部在 raw script 里，必须按 txid 拉原始字节流去解析 prevout 链、维护资产状态。
- **构造交易**：钱包 / SDK / 业务侧在拼接新交易时，需要按 txid 反查父交易的精确序列化字节，用于填充 input、计算 sighash、构造关联交易（FT 转账、Pool 撮合、HTLC 解锁等）。
- **签名 / 联签 / 资产托管**：对方只给一个 txid（或父交易 txid），就要求拿到精确字节去验签、补签、出具凭证。
- **跨链桥 / Watcher / SPV 回查**：客户端已知某个 txid 应当存在，向全节点反查 raw 以核验状态。
- **广播后回查 / 重广播循环**：客户端发完 `sendrawtransaction` 之后，靠 `getrawtransaction(txid, 0)` 反复确认它还在 mempool / 已经上链。
- **任何"已知 txid 就要看到字节"的运维 / 审计 / 监控查询**。

这类调用的共同特征：

1. **极高频**——几十~几百个客户端长跑；
2. **只读**——不改变节点状态；
3. **不需要链上下文**（不关心 confirmations / blockhash / blocktime / blockheight）；
4. **不能用替代手段绕过**——`gettransaction` 是钱包接口，REST `/rest/tx/<txid>.bin` 只覆盖 disk，不覆盖 mempool；ZMQ 推送只在事件发生那一刻有效，不支持回查。

也就是说：**只要 TBC 主网在跑，这条 RPC 路径就一定被持续打**。任何让它持有 `cs_main` 的设计，都会把它们的并发瓶颈强行串到全节点验证主路径上。

---

## 2. 现状：旧路径的代价

旧实现（`getrawtransaction` 入口）一进函数就 `LOCK(cs_main)`，无论 `verbose` 是 true 还是 false。`verbose=false`（业务方 99% 的用法）实际**只需要**：

| 数据 | 来源 | 是否需要 cs_main |
|------|------|------------------|
| mempool 中的 raw tx | `CTxMemPool::Get` | 否，靠 `mempool.smtx` 自带的读写锁 |
| 已上链的 raw tx | `pblocktree->ReadTxIndex` + `blkXXXXX.dat` | 否，LevelDB 自身线程安全；块文件**仅追加写**，旧字节永不重写 |

可旧代码把这些都套在 `cs_main` 下：

- **`cs_main` 是节点全局大锁**，验证主路径 `ProcessNewBlock` / `ConnectTip` / `ActivateBestChainStep` / 重组 / `processValidation` 全部在它上面。
- **`getrawtransaction` 哪怕只读也要排队**——大量只读请求在 `cs_main` 上拥塞，反过来抬高验证主路径的尾延迟。
- 即便已经走到了 mempool 命中或 LevelDB 命中分支，`cs_main` 仍然在该 RPC 整个生命周期中持有，毫无必要。

附带，`CTxMemPool::Get` 在改造前用的是 `std::unique_lock`（独占），多个只读 RPC 之间会互相阻塞，这同样不必要——`mempool.smtx` 是 `std::shared_mutex`，`Get` 本质是只读，应当走共享读。

---

## 3. 改动清单（最小化、可独立 cherry-pick）

### 3.1 新增 `GetTransactionRaw`（`src/validation.{h,cpp}`）

一个**专门的、cs_main-free 的**只读 helper。语义：

1. 先查 `mempool.Get(txid)`——只锁 `mempool.smtx`；
2. 若 `-txindex` 开启，查 `pblocktree->ReadTxIndex` + `blkXXXXX.dat` 反序列化；
3. 不计算 `hashBlock`、不计算 `isGenesisEnabled`、不走 `fAllowSlow` 的 UTXO 扫描分支；
4. 命中则返回 `CTransactionRef`，未命中或异常返回 `false`。

```cpp
// validation.h
bool GetTransactionRaw(const TxId &txid, CTransactionRef &txOut);
```

> 注意：这并不替换 `GetTransaction(...)`。`GetTransaction` 被保留并继续承担需要链上下文的所有调用方（`verbose=true`、其他 RPC、wallet 等）。

### 3.2 `getrawtransaction` 拆成两条路径（`src/rpc/rawtransaction.cpp`）

- `verbose=false` 分支：解析参数 → `GetTransactionRaw` → 输出 hex。**全程不持 `cs_main`**。
- `verbose=true` 分支：保持原行为不变——`LOCK(cs_main)`，`GetTransaction(... hashBlock, isGenesisEnabled)`，按 `mapBlockIndex` / `chainActive` 计算 `confirmations` / `blocktime` / `blockheight`。

外部 JSON 行为对 `verbose=false` 客户端字节级一致（同样输出 `{"result": "<hex>", "error": null, "id": ...}`）。

### 3.3 `CTxMemPool::Get` 改 `shared_lock`（`src/txmempool.cpp`）

```cpp
CTransactionRef CTxMemPool::Get(const uint256 &txid) const {
-    std::unique_lock lock(smtx);
+    std::shared_lock lock(smtx);
     return GetNL(txid);
}
```

- `GetNL` 仅做 `mapTx.find` 并复制 `shared_ptr`，**纯读**；
- 与同文件中 `InfoAllNL` 等只读路径的 `shared_lock` 用法保持一致；
- 写路径（`AddTx` / `RemoveTxRecursive` 等）仍然是 `unique_lock`，互斥语义不变。

---

## 4. 安全性论证（核心）

> 对评审而言，这是文档最重要的一节。

### 4.1 mempool 读路径

- `mempool.smtx` 是 `std::shared_mutex`，`AddTx` / `RemoveTxRecursive` / `Clear` 等所有写入都已持 `unique_lock`；
- `GetNL` 在 `shared_lock` 下做 `unordered_map::find` 并返回 `CTransactionRef`（`std::shared_ptr<const CTransaction>`），**只读 + 引用计数**，不会被写线程并发 invalidate；
- 即使并发地 `RemoveTxRecursive` 把条目从 `mapTx` 删了，调用方手里的 `shared_ptr` 仍然指向不可变的 `CTransaction` 对象——这正是 `CTransactionRef` 设计为 `shared_ptr<const CTransaction>` 的原因；
- 因此本路径下没有 `cs_main` 的语义不再被破坏。

### 4.2 已上链 raw bytes 读路径（disk 路径）

- **LevelDB（`pblocktree`）线程安全**：LevelDB 的 `Get` / `Iterator` 显式支持多线程并发读，TBC 节点在其他位置（如 `txindex` 写入与查询）已经依赖该保证。
- **块文件（`blkXXXXX.dat`）只追加**：除了 prune 模式下的整文件删除，区块文件的写入是 append-only，旧偏移的字节永不被重写——这是 Bitcoin 全节点几十年验证下来的不变量。`CDiskTxPos` 拿到的 `(file, offset)` 一旦成功落库，对应字节流就不会再变。
- **prune 模式**：在 `-prune` 模式下，老文件可能被整体删除。`OpenBlockFile` 已经处理"打不开"的失败路径（旧的 `GetTransaction` 也是同样的失败语义），改造后在 `GetTransactionRaw` 里同样以 `file.IsNull()` → `return false` 处理，对调用方表现为"找不到"，与现有错误模型一致。
- **反序列化阶段**：`CAutoFile >> diskTx` 的解码在 try/catch 内；任何 I/O 或解码异常都会被吞掉并返回 `false`。同时保留了 `diskTx->GetId() != txid` 的一致性检查，避免误读返回错误数据。

### 4.3 我们**没有**做的事

- 没有跳过 `txid` 一致性校验（命中后仍校验 `diskTx->GetId() == txid`）。
- 没有跳过 `-txindex` 检查（`fTxIndex == false` 时直接走"未命中"分支）。
- 没有去掉 `verbose=true` 路径上的 `cs_main`——任何需要 `mapBlockIndex` / `chainActive.Height()` / `IsGenesisEnabled` 的字段，仍然在 `cs_main` 下计算。
- 没有改任何写路径锁、没有动 mempool 的写顺序、没有动 PTV、没有动 ConnectBlock。
- 没有改变 `getrawtransaction` 在 `verbose=true / verbose=false` 下的对外 JSON 形状。

### 4.4 与重组 / 回滚的交互

- 重组发生时：`mempool` 会经 `removeForReorg`（写锁）做修整，块文件不会被重写；
- 极端竞态：旧实现下 `verbose=false` 拿到的就是"调用瞬间的状态"，新实现同样如此——只是把锁粒度从 `cs_main` 收窄到 `mempool.smtx`。
- 调用方语义不变：不论旧路径还是新路径，"我刚拿到 raw bytes"都不蕴含"该交易此刻仍在最佳链上"。需要这种保证的客户端原本就必须用 `verbose=true` 或单独查 `getblockheader` / `gettxoutproof`。

### 4.5 `CTxMemPool::Get` 改 shared_lock 的安全性

- 写者全部用 `unique_lock`，`shared_mutex` 保证读写互斥；
- 多读并发是 `shared_mutex` 的设计目的；
- 该函数本身只做哈希表 `find` + `shared_ptr` 拷贝，没有副作用、没有迭代器外泄。

---

## 5. 有效性论证

### 5.1 直接收益

- 移除了 `verbose=false` 路径上的全局锁——本来 N 个客户端互相串行 + 与验证主路径互相串行，现在只与 mempool 写者短暂互斥（毫秒级）。
- 多个并发"取 raw"请求之间**真正并行**（shared_lock 共存）。
- 验证主路径的 `cs_main` 持锁尾延迟不再被高频只读 RPC 抬升。

### 5.2 系统层面

- 与 `docs/plans/cs_main-refactor-plan.md` 的方向一致：把"不需要全局视图"的读出走捷径，让 `cs_main` 重新成为"严肃的链状态变更锁"。
- 给后续"FT 子孙链真并行验证"等改造扫掉前置噪音——索引器、签名服务的回查不再是验证路径的隐形对手。

---

## 6. 测试覆盖

围绕 `getrawtransaction(txid, verbose=false)` 的新读路径，覆盖：

1. **mempool 命中路径**：返回值与改造前字节级一致；
2. **不开 -txindex 也能从 mempool 命中**；
3. **txindex on-disk 路径**（挖出后）：返回值与挖出前 mempool 的 hex 一致；
4. **未开 -txindex 且已落盘**：返回明确的 "No such mempool transaction. Use -txindex..." 错误；
5. **未知 txid**：返回 "No such mempool or blockchain transaction" 错误。

`verbose=true` 路径行为不变，回归由现有 `rpc_rawtransaction.py` 等既有测试承担。

---

## 7. 兼容性

- **RPC ABI**：`getrawtransaction` 在所有现有参数组合下输出与改前一致（包括 `verbose=true` 的 JSON 形状、字段名和异常信息）。`verbose=false` 仅是内部读路径切换，外部 JSON 字节级一致。
- **不引入新接口**：本改动只改 `getrawtransaction` 内部实现，不新增任何 RPC。
- **配置**：不新增 CLI flag、不新增 datadir 文件。
- **数据库 / 磁盘格式**：不变。
- **P2P / 共识**：完全无关。

---

## 8. 评审清单

- [ ] `GetTransactionRaw` 没有读取 `mapBlockIndex` / `chainActive` / `pcoinsTip` 任意一项 → 核对 `validation.cpp` 实现。
- [ ] `getrawtransaction` 在 `verbose=true` 分支仍 `LOCK(cs_main)` 且后续逻辑不变。
- [ ] `getrawtransaction` 在 `verbose=false` 分支不再持有 `cs_main`，且对外 JSON 与改前字节级一致。
- [ ] `CTxMemPool::Get` 切回 shared_lock 后，仅有的 callers（含本改动）都没有借此做"读后改"的事情。
- [ ] prune / 重组 / 节点重启的失败路径都返回 `false`，不会抛异常穿透到 RPC handler。
