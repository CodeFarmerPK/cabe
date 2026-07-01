# Cabe P7-M2 设计:写路径入 reactor

> 本里程碑把**写路径(Put/Delete)与运营口(SetWalLevel/Snapshot)整段从 Engine 迁进 per-reactor
> 事件循环**。M1 已建好异步机制(投递→挂起→reactor 单线程处理→唤醒,全程无 mutex),M2 在同一机制上
> 把写执行体接进去:调用线程校验后投递写 op,reactor 线程跑 `Acquire→pool→memcpy/CRC/时间戳→io.Write
> →WriteWal→Insert→回收→MaybeRequestSnapshot`。仍是 **N=1、单 reactor、单调用线程**。完成后 M1 那
> 39 个"预期红"的写路径用例全部转绿,引擎行为等价于 P6 的旧同步引擎。
>
> **本文为详细设计**,grill 的全部裁决(A1–A5 直接采用 + B1–B6 逐条讨论)汇集于此,统一编号 P7M2-D1~D11。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P7 / M2 |
| 状态 | ✅ **已实装**(P7M5 收敛) |
| 上游依赖 | P7M1 完成(reactor 异步机制、读路径);P7 阶段计划(doc/P7/README.md,P7-D1~D13);doc/P7/P7M1_reactor_skeleton_design.md |
| 下游依赖本里程碑 | P7M3(单设备多调用线程:入站队列真多生产者 + §5.3 三竞态偿还)→ M4(多设备 + 运营口真广播)→ M5 |
| 退出判定 | 见 §10 |

---

## 1. 目标与范围

### 1.1 目标

1. **写路径端到端**:Put / Delete 经 reactor 执行,调用线程拿到等价于旧同步引擎的结果。
2. **运营口入 reactor**:SetWalLevel / Snapshot 降格为 op,**广播到所有 reactor**(N=1 即投给唯一一个)。
3. **per-reactor 配置**:每个 reactor 持一份 `Options` 拷贝,`wal_level` 成 per-reactor 可变态、消跨 reactor
   读写 race(为 M4 铺好结构);其余字段 Open 后只读。
4. **写路径辅助迁入 reactor**:`DoSnapshot`/`WriteWalRescuing`/`MaybeRequestSnapshot`/`RequestSnapshot`/
   `TrimDeviceBlock` 与 Put/Delete 核心一并迁入,**share-nothing 落到文件边界**(线程边界 = 文件边界)。
5. **测试复活 + 补强**:39 个写路径用例零改写复活(等价性/回归证明);reactor_test 加 Put-Get 命中(首次
   跑通 `ExecuteGet` 取数路径)+ 写路径压测(sync+TSAN 下验写交接 race-free)。

### 1.2 交付范围

1. **`engine/reactor.h`**(修改):`OpType` 扩 `Put/Delete/SetWalLevel/Snapshot`;`OpNode` 加 `value`
   (DataView 输入)、`new_level`(WalLevel 输入);`Reactor` 加 `Options options_` 成员(声明在 `dc_` 之前)、
   构造签名加 `const Options&`、四个写执行体 + 五个迁入辅助方法的私有声明。
2. **`engine/reactor.cpp`**(修改,主战场):`Run` 分派改 switch;`ExecutePut`/`ExecuteDelete`/
   `ExecuteSetWalLevel`/`ExecuteSnapshot`;迁入 `DoSnapshot`/`WriteWalRescuing`/`MaybeRequestSnapshot`/
   `RequestSnapshot`/`TrimDeviceBlock`(改以 `dc_`/`options_` 为隐式对象);构造体内 rebind 三个 opts 指针。
3. **`engine/engine.h`**(修改):删掉写路径私有方法声明(`DoSnapshot`/`WriteWalRescuing`/
   `RequestSnapshot`/`MaybeRequestSnapshot`/`TrimDeviceBlock`),它们迁去 reactor;恢复链声明不变。
4. **`engine/engine.cpp`**(修改):`Put`/`Delete` 实装(校验 + 路由 + submit + wait);`SetWalLevel`/
   `Snapshot` 实装(校验 + 广播 `reactors_` + 取首错);Open 第二阶段 `make_unique<Reactor>(std::move(dc), options_)`;
   删写路径辅助函数体(迁去 reactor)。
5. **`wal/wal.h`、`snapshot/snapshot.h`、`io/sync/sync_io_backend.h`、`io/uring/io_uring_backend.h`**(均仅改头文件):
   各在类内加一行 header-inline setter `void RebindOptions(const Options* opts) noexcept { opts_ = opts; }`,不触碰各自 .cpp。
6. **`test/engine/reactor_test.cpp`**(修改):加 Put-Get 命中测 + 写路径压测/活性测。

**零新增 / 不动**:**无新增源文件、无新增错误码 → `engine/CMakeLists.txt` 与 `common/error_code.h` 不动**;
group commit 机制(P7-D10)、盘上格式、恢复链逻辑、index/slots 内部一律不碰。

### 1.3 明确不做(各有归处)

| 不做项 | 归处 |
|---|---|
| 多调用线程并发(入站真多生产者、§5.3 三竞态偿还) | P7M3 |
| 真实路由 `hash%N` / 并行广播 / 多 reactor 失败原子性 | P7M4 |
| group commit 拆壳 / 真批量 fdatasync / RWF_DSYNC | 性能轮(P7-D10;reactor 单写者直调现有 `WriteWal`、自任 leader 批 1) |
| 流水线及其债(committed_seq / 跨线程 BufferPool / 模糊快照 / 同 key 乱序) | 性能轮(P7-D13;M2 的 pool 是 reactor 私有单线程,非跨线程) |
| WAL 定时刷(`wal_flush_interval_ms`) | 后续轮(旧引擎本无;落地需把 `Run` 无限期等待改带超时) |
| TRIM(`TrimDeviceBlock` 仍空) | P7 全程不做(P7-D13;迁入后保留空方法占位) |
| 运行时 I/O 故障注入 | no-fault-injection(保留防御返回、不测;`kWalFull` 是资源状态、照常真测) |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 | grill 来源 |
|---|---|---|---|
| **P7M2-D1** | 交付边界 | 写路径(Put/Delete)+ 运营口(SetWalLevel/Snapshot)入 reactor;仍 N=1 单 reactor 单调用线程;39 个写路径用例复活、行为等价旧引擎 | — |
| **P7M2-D2** | 写路径归属 | **迁入 Reactor**:Put/Delete 核心 + `DoSnapshot`/`WriteWalRescuing`/`RequestSnapshot`/`MaybeRequestSnapshot`/`TrimDeviceBlock` 全变 Reactor 私有方法、以 `dc_`/`options_` 为隐式对象;恢复链 + 生命周期编排留 Engine。**线程边界=文件边界**,share-nothing 结构化 | B1 |
| **P7M2-D3** | per-reactor 配置拷贝 | 每 Reactor 一份完整 `Options options_`(声明在 `dc_` 之前);`io`/`wal`/`snapshot` 各加 `RebindOptions`,构造时三个 `opts_` 指针重指本 reactor 副本;`wal_level` 普通字段、**无 atomic 无锁**(读写都在属主 reactor 单线程) | B2 |
| **P7M2-D4** | OpType / OpNode 扩展 | `OpType` 加 `Put=3/Delete=4/SetWalLevel=5/Snapshot=6`,运营口与读写**共用同一 inbox**;`OpNode` 平铺加 `DataView value`、`WalLevel new_level`,不用 union | A1+A2 |
| **P7M2-D5** | 校验上移 + 错误码 | Put 的 `opened_`/`key.empty`/`value.size`/`key>kWalKeyMax`、Delete 的 `opened_`/`key.empty`、SetWalLevel 的 `opened_`/`IsValidWalLevel` 全在**调用线程 fail-fast**;Delete 的"键是否存在"留执行体内(需索引)。**零新增错误码**(复用现有;非法档 → `kEngineInvalidOpts`) | A3+A4 |
| **P7M2-D6** | Put / Delete 执行体 | 逐字照搬 P6M4 逻辑(见 §5),改 `devices_[i]`→`dc_`、入参来自 `op`;保留旧块回收时序、CRC、时间戳;`SubmitAndWait` 原样复用(写 op 无输出 buffer,result 只发布 rc) | B1+A1 |
| **P7M2-D7** | 自动快照 + 快照辅助 | `MaybeRequestSnapshot` 在 `ExecutePut`/`ExecuteDelete` 尾段、reactor 线程同步跑(读 `options_.snapshot_threshold_bytes`);手动 `Snapshot` op → `DoSnapshot`;同线程无跨线程,行为等价旧引擎(触发快照的那次写延迟含快照写,性能不卡门槛) | A5 |
| **P7M2-D8** | SetWalLevel 语义 | `ExecuteSetWalLevel` 逐字照搬旧收紧逻辑:`!IsWalSyncLevel(old) && IsWalSyncLevel(new) → dc_.wal.Flush() → options_.wal_level=new`,**只管 WAL 维度、保行为**;`io`/`wal` 的 `opts_` 指 `options_`,下次操作现读到新值 | B3 |
| **P7M2-D9** | 运营口广播 | SetWalLevel/Snapshot **遍历 `reactors_` 顺序广播 + 取首错**,每轮一个栈 `OpNode` 复用 `SubmitAndWait`(不堆分配、不写多 op 等待器);并行广播 + 多 reactor 失败原子性留 M4(N=1 无法触发) | B4 |
| **P7M2-D10** | 刷盘触发 | 沿用现有四触发(缓冲满 / Close / 收紧档 / 快照前),全在 reactor 线程;定时刷继续延后(旧引擎本无,保行为);攒批档崩溃丢失窗口由非定时触发兜底,读己之写不受影响 | B5 |
| **P7M2-D11** | 测试边界 | 38 个零改写复活当等价性/回归证明;FlushOnTighten 经 D8 复活;reactor_test 加 Put-Get 命中 + 写路径压测,recover 带数据交给 recovery_test;退出 = sync(Debug)+sync/TSAN+io_uring/ASAN 全量全绿 + 覆盖率含写路径 | B6 |

---

## 3. 核心结构

### 3.1 OpType / OpNode 扩展(`engine/reactor.h`)

```cpp
// M2:加 Put/Delete/SetWalLevel/Snapshot。运营口与读写共用同一 inbox(单消费者天然串行)。
enum class OpType : std::uint8_t {
    Get = 1, Stop = 2, Put = 3, Delete = 4, SetWalLevel = 5, Snapshot = 6,
};

struct OpNode {
    OpType type;
    std::string_view key;                          // Get/Put/Delete 输入
    DataView value;                                // Put 输入(M2 新增;caller DataView,视图不拷贝)
    DataBuffer out;                                // Get 输出
    WalLevel new_level = WalLevel::WalSync;        // SetWalLevel 输入(M2 新增)
    std::atomic<std::int32_t> result{kOpPending};
    OpNode* next = nullptr;
    std::atomic<std::uint32_t>* wake = nullptr;
};
```

> `OpNode` 是调用线程栈变量、同一时刻只跑一个 op,字段复用无歧义;平铺字段比 union 更易读、与 M1 风格一致
> (P7M2-D4)。写 op(Put/Delete/SetWalLevel/Snapshot)不写 `out`,结果只经 `result` 发布一个 rc。
> 需 `#include "engine/options.h"`(取 `WalLevel`)——`device_context.h` 已间接带入,显式补一行更稳。

### 3.2 Reactor 类新形态(`engine/reactor.h`)

```cpp
class Reactor {
public:
    Reactor(DeviceContext&& dc, const Options& opts);   // M2:多收一份 Options 拷贝
    ~Reactor();
    // 不可拷贝、不可移动(同 M1)

    std::int32_t Start();
    std::int32_t Stop();
    void Submit(OpNode* op) noexcept;

private:
    void Run();
    std::int32_t ExecuteGet(OpNode* op);
    std::int32_t ExecutePut(OpNode* op);            // M2
    std::int32_t ExecuteDelete(OpNode* op);         // M2
    std::int32_t ExecuteSetWalLevel(OpNode* op);    // M2
    std::int32_t ExecuteSnapshot(OpNode* op);       // M2
    // 写路径辅助(M2 从 Engine 迁入,以 dc_/options_ 为隐式对象)
    std::int32_t DoSnapshot();
    std::int32_t WriteWalRescuing(const WalEntry& e);
    void RequestSnapshot();
    void MaybeRequestSnapshot();
    void TrimDeviceBlock(BlockId id);               // 仍空(P7 不做 TRIM)
    // finalize / 入站栈 / close(同 M1)
    void Finalize(OpNode* op, std::int32_t rc) noexcept;
    void FailWakeChain(OpNode* head) noexcept;
    void CloseDc() noexcept;
    OpNode* DrainAll() noexcept;
    static OpNode* Reverse(OpNode* head) noexcept;

    std::atomic<OpNode*> inbox_{nullptr};
    std::thread thread_;
    Options options_;                               // M2:per-reactor 配置拷贝(声明在 dc_ 之前,见 §4)
    DeviceContext dc_;
    std::int32_t close_result_ = 0;
    bool dc_closed_ = false;
};
```

> **成员声明序**:`options_` 在 `dc_` **之前**。成员按声明序构造、逆序析构 → `dc_` 先析构、`options_` 后析构,
> 保证 `dc_` 内 `io`/`wal`/`snapshot` 指向 `options_` 的 `opts_` 指针在 `dc_` 整个生命期(含析构)都有效。

`SubmitAndWait(Reactor&, OpNode&)`(M1 那只)**原样复用**,对任何 op 类型都成立——它只投递并等 `result`
脱离 `kOpPending`,不关心 op 是读是写(P7M2-D6)。

---

## 4. per-reactor 配置拷贝与 rebind(P7M2-D3)

### 4.1 问题:dc 里有三个 `opts_` 指针读配置

核实(grep)`dc` 中持 `const Options*` 的组件有**三个**,且两个现读 `wal_level`:

| 组件 | 持有 | 读什么 |
|---|---|---|
| `dc.io`(IoBackend) | `sync_io_backend.h` / `io_uring_backend.h` 的 `opts_` | `wal_level` → `IsValueFuaLevel` 决定 value 是否 FUA |
| `dc.wal`(Wal) | `wal.h:180` 的 `opts_` | `wal_level` → `IsWalSyncLevel` 决定同步/攒批 |
| `dc.snapshot`(Snapshot) | `snapshot.h:82` 的 `opts_` | 只读 `snapshot_buffer_size`(**不读 wal_level**) |

三者目前在 Open 第一阶段被设成 `&Engine::options_`(`engine.cpp:56/74/76` create 分支的 io/wal/snapshot Open + `engine.cpp:211/223` RecoverDevice 内的 snapshot/wal Open)。M2 要把它们重指到
本 reactor 的配置拷贝。

### 4.2 为什么 M2 就要上拷贝(N=1 本可不上)

N=1 时 `SetWalLevel` 是投给该 reactor 的 op、在 reactor 线程改 `wal_level`,读点(`io.Write`/`WriteWal`)也
都在 reactor 线程,**同线程串行、本无 race**。仍要现在上拷贝,因为:

1. **D2(迁入)逼出来**:迁入的执行体/辅助要读一份配置。若不建拷贝,只能读回 `Engine::options_`——等于在
   reactor 里留一根指回 Engine 的引用,把 D2 的 share-nothing 又破了。
2. **M4 免返工**:N>1 时 N 根 reactor 线程会同时读、`SetWalLevel` 会从 reactor 线程写共享 `Engine::options_.wal_level`
   ——正是 P7-D8 要消灭的跨 reactor race。现在上拷贝,M4 在配置模型上零改动。

### 4.3 rebind 机制

各组件加一行 setter(只是重指它本就持有的指针):

```cpp
// Wal / Snapshot / SyncIoBackend / IoUringIoBackend 各加
void RebindOptions(const Options* opts) noexcept { opts_ = opts; }
```

Reactor 构造时整体拷贝 + 重指三个指针:

```cpp
Reactor::Reactor(DeviceContext&& dc, const Options& opts)
    : options_(opts), dc_(std::move(dc)) {          // options_ 声明在前,先构造
    dc_.io.RebindOptions(&options_);
    dc_.wal.RebindOptions(&options_);
    dc_.snapshot.RebindOptions(&options_);          // snapshot 只读不变字段,一并重指立"统一只读自己那份"不变式
}
```

Engine 第二阶段(§7)`make_unique<Reactor>(std::move(recovered[i]), options_)`;第一阶段仍用
`&Engine::options_` 打开三组件(那时 reactor 尚不存在、恢复期只读、无妨),move 进 reactor 后由构造体一次性
rebind 到拷贝。

### 4.4 为什么不要 atomic/锁

整体拷贝 `Options`(很小,含 `devices` 小 vector);**只有 `wal_level` 会在 Open 后变**,其余是只读拷贝。
`wal_level` 普通字段即可:它只被属主 reactor 一根线程碰——`SetWalLevel` op 写、`io`/`wal` 读,**都在该 reactor
线程、按 inbox 串行**。不是靠锁/原子消 race,是靠"读写同线程串行"。调用线程从不读 `wal_level`(Put 的 caller
侧校验只读常量),无残留竞争面。

---

## 5. 写路径执行体(P7M2-D6)

### 5.1 Run 分派改 switch

```cpp
// Stop 判断(同 M1 的 drain-then-close)之后:
std::int32_t rc;
switch (op->type) {
    case OpType::Get:         rc = ExecuteGet(op);         break;
    case OpType::Put:         rc = ExecutePut(op);         break;
    case OpType::Delete:      rc = ExecuteDelete(op);      break;
    case OpType::SetWalLevel: rc = ExecuteSetWalLevel(op); break;
    case OpType::Snapshot:    rc = ExecuteSnapshot(op);    break;
    default:                  rc = err::kEngineNotImplemented; break;
}
Finalize(op, rc);   // result.store(release) + 唤醒该 caller 的 WakeSlot.gen(同 M1,扣 last-touch)
```

> 写 op 的"输出"只有 rc,`Finalize` 的 `result.store(release)` 即发布之;`ExecutePut` 内对 `dc_` 的所有改写
> 也都经这个 release 对调用线程可见。内存序与 M1 读路径一致,无新增(§11.2)。

### 5.2 ExecutePut(逐字照搬 P6M4,改 dc_/op)

```cpp
std::int32_t Reactor::ExecutePut(OpNode* op) {
    BlockId block_id{};
    std::int32_t rc = dc_.block_allocator.Acquire(&block_id);
    if (rc != err::kSuccess) return rc;                                   // kEngineNoSpace
    std::byte* buf = dc_.pool.Allocate();
    if (!buf) { dc_.block_allocator.Recycle(block_id); return err::kEnginePoolExhausted; }
    std::memcpy(buf, op->value.data(), kValueSize);
    const std::uint32_t value_crc = util::CRC32(op->value);
    const std::uint64_t now       = util::GetWallTimeNs();
    rc = dc_.io.Write(block_id.block_idx(), buf);                         // FUA 由 io 读 opts_->wal_level 定
    dc_.pool.Free(buf);
    if (rc != err::kSuccess) { dc_.block_allocator.Recycle(block_id); return rc; }
    rc = WriteWalRescuing(WalEntry{WalEntryType::Put, op->key, block_id, value_crc, now});
    if (rc != err::kSuccess) { dc_.block_allocator.Recycle(block_id); return rc; }
    ValueMeta old_meta{};
    const bool had_old = (dc_.meta_index.Lookup(op->key, &old_meta) == err::kSuccess);
    ValueMeta meta{};
    meta.block = block_id; meta.timestamp = now; meta.crc = value_crc; meta.state = ValueState::Active;
    dc_.meta_index.Insert(op->key, meta);
    if (had_old) { dc_.block_allocator.Recycle(old_meta.block); TrimDeviceBlock(old_meta.block); }
    MaybeRequestSnapshot();
    return err::kSuccess;
}
```

时序照搬要点:**io.Write 失败回收新块;WriteWal 失败回收新块**(撞墙救援见 §5.5);**WAL 写成功后才 Insert**;
**覆盖写在 Insert 之后回收旧块**(`WriteWalRescuing` 内的救援快照发生在 Insert 之前,故救援快照不含本笔在飞 key
——保持原序、原行为)。

### 5.3 ExecuteDelete

```cpp
std::int32_t Reactor::ExecuteDelete(OpNode* op) {
    ValueMeta meta{};
    std::int32_t rc = dc_.meta_index.Lookup(op->key, &meta);
    if (rc != err::kSuccess) return rc;                                   // kIndexKeyNotFound:不存在就不写 WAL
    rc = WriteWalRescuing(WalEntry{WalEntryType::Delete, op->key, BlockId{}, 0, util::GetWallTimeNs()});
    if (rc != err::kSuccess) return rc;                                   // WAL 失败不动内存
    dc_.meta_index.Delete(op->key);
    dc_.block_allocator.Recycle(meta.block);
    TrimDeviceBlock(meta.block);
    MaybeRequestSnapshot();
    return err::kSuccess;
}
```

与 Put 的区别:Delete **不碰 `dc_.io`/`dc_.pool`**(无数据写盘),旧块直接回收。

### 5.4 迁入的快照/回收辅助(改读 `dc_`/`options_`)

```cpp
void Reactor::TrimDeviceBlock(BlockId id) { (void)id; /* TODO(性能轮): 待 TRIM 队列异步 BLKDISCARD */ }

void Reactor::MaybeRequestSnapshot() {
    const std::uint64_t grown =
        (dc_.wal.last_seq() - dc_.snapshot.last_trigger_seq()) * kWalFrameSize;
    if (grown >= options_.snapshot_threshold_bytes) RequestSnapshot();   // 读本 reactor 自己的配置
}

void Reactor::RequestSnapshot() {
    std::int32_t rc = DoSnapshot();
    if (rc != err::kSuccess)
        CABE_LOG_ERROR("自动快照失败: rc=%d(不影响本次写,按退避重试)", rc);
}

std::int32_t Reactor::DoSnapshot() {
    dc_.snapshot.NoteTriggerAttempt(dc_.wal.last_seq());
    std::int32_t rc = dc_.wal.Flush();
    if (rc != err::kSuccess) return rc;
    const std::uint64_t covered_seq = dc_.wal.last_seq();
    const std::uint64_t boundary    = dc_.wal.reclaim_boundary();        // 与 covered_seq 同刻、Flush 之后
    rc = dc_.snapshot.Write(covered_seq,
        [&](const MetaIndexVisitor& v) { return dc_.meta_index.ForEach(v); });
    if (rc != err::kSuccess) return rc;                                   // 失败:boundary 丢弃,绝不回收
    const std::int32_t rrc = dc_.wal.ReclaimUpTo(boundary);
    if (rrc != err::kSuccess)
        CABE_LOG_ERROR("WAL 回收失败(快照已成功,空间暂不复用): rc=%d", rrc);
    return err::kSuccess;
}
```

### 5.5 WriteWalRescuing(撞墙救援,单写者下"重试恰一次必成")

```cpp
std::int32_t Reactor::WriteWalRescuing(const WalEntry& e) {
    std::int32_t rc = dc_.wal.WriteWal(e);
    if (rc != err::kWalFull) return rc;                                   // 正常路径零开销:就一个比较
    CABE_LOG_WARN("WAL 环已满,强制快照腾空间");
    rc = DoSnapshot();
    if (rc != err::kSuccess) return err::kWalFull;                        // 救不了,对外返运维信号
    return dc_.wal.WriteWal(e);                                           // 重试恰一次(Open 验过 ring≥buffer+4K)
}
```

> reactor 是该 dc 的唯一写者,`kWalFull → DoSnapshot → 重试`全程串行无并发干扰,"重试恰一次必成"前提
> (Open 时 `ring_size ≥ 缓冲+4K`)在单写者下成立(P7-D10/M2 README)。`kWalFull` 是资源状态、照常真测。

---

## 6. 运营口(P7M2-D7/D8/D9)

### 6.1 ExecuteSetWalLevel(收紧先刷,改本 reactor 的拷贝)

```cpp
std::int32_t Reactor::ExecuteSetWalLevel(OpNode* op) {
    const WalLevel old = options_.wal_level;                              // 本 reactor 自己那份
    // 收紧(攒批档 2/4 → 同步档 1/3):先把本 reactor 攒批缓冲刷净,新保证从此刻成立
    if (!IsWalSyncLevel(old) && IsWalSyncLevel(op->new_level)) {
        std::int32_t rc = dc_.wal.Flush();
        if (rc != err::kSuccess) return rc;
    }
    options_.wal_level = op->new_level;                                   // io/wal 的 opts_ 指此,下次操作现读到
    return err::kSuccess;
}
```

校验(`IsValidWalLevel`)已在调用线程做(§7),执行体假定入参合法。**只管 WAL 维度、不碰 value FUA**——
逐字照搬旧 `Engine::SetWalLevel`,P7 保行为(放松不刷;FUA 维度不补刷数据盘,既有行为)。

### 6.2 ExecuteSnapshot

```cpp
std::int32_t Reactor::ExecuteSnapshot(OpNode* /*op*/) { return DoSnapshot(); }
```

### 6.3 Engine 侧广播(顺序 + 取首错)

SetWalLevel / Snapshot **遍历 `reactors_` 顺序广播**,每轮一个栈 `OpNode` 复用 `SubmitAndWait`,取首错。
N=1 即一圈,与"只投 `reactors_[0]`"等价;N>1 时各 reactor 各执行各的。**并行广播(投全部再等齐,快照可跨设备
并发)与多 reactor 失败原子性(部分应用 / 回滚)留 M4**——N=1 无法触发。栈节点逐轮复用安全靠 last-touch 条款
(reactor `result.store` 后只碰该 caller 的 WakeSlot.gen(进程级长寿槽)、不再碰 OpNode,§11.1)。

---

## 7. Engine 层改造

```cpp
Status Engine::Put(std::string_view key, DataView value) {
    if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
    if (key.empty())                  return Status::Error(err::kMemEmptyKey);
    if (value.size() != kValueSize)   return Status::Error(err::kEngineInvalidValue);
    if (key.size() > kWalKeyMax)      return Status::Error(err::kWalKeyTooLong);
    OpNode op{}; op.type = OpType::Put; op.key = key; op.value = value;
    const std::int32_t rc = SubmitAndWait(*reactors_[RouteKey(key)], op);
    return rc == err::kSuccess ? Status::Ok() : Status::Error(rc);
}

Status Engine::Delete(std::string_view key) {
    if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    OpNode op{}; op.type = OpType::Delete; op.key = key;
    const std::int32_t rc = SubmitAndWait(*reactors_[RouteKey(key)], op);
    return rc == err::kSuccess ? Status::Ok() : Status::Error(rc);
}

Status Engine::SetWalLevel(WalLevel lvl) {
    if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
    if (!IsValidWalLevel(lvl))                    return Status::Error(err::kEngineInvalidOpts);
    std::int32_t first = err::kSuccess;
    for (auto& r : reactors_) {                                           // 广播 + 取首错
        OpNode op{}; op.type = OpType::SetWalLevel; op.new_level = lvl;
        const std::int32_t rc = SubmitAndWait(*r, op);
        if (rc != err::kSuccess && first == err::kSuccess) first = rc;
    }
    return first == err::kSuccess ? Status::Ok() : Status::Error(first);
}

Status Engine::Snapshot() {
    if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
    std::int32_t first = err::kSuccess;
    for (auto& r : reactors_) {
        OpNode op{}; op.type = OpType::Snapshot;
        const std::int32_t rc = SubmitAndWait(*r, op);
        if (rc != err::kSuccess && first == err::kSuccess) first = rc;
    }
    return first == err::kSuccess ? Status::Ok() : Status::Error(first);
}
```

- `Put`/`Delete` 按 `RouteKey` 路由到**单个** reactor(M1 起 `RouteKey` 返 0);`SetWalLevel`/`Snapshot`
  **广播**到全部。
- **Open 第二阶段**:`auto r = std::make_unique<Reactor>(std::move(recovered[i]), options_);`(多传 `options_`)。
  第一阶段恢复链不变(裸 dc、`&options_`)。
- `engine.h`:删 `DoSnapshot`/`WriteWalRescuing`/`RequestSnapshot`/`MaybeRequestSnapshot`/`TrimDeviceBlock`
  五个声明(迁去 reactor);`engine.cpp` 删其函数体。恢复链声明/实现不变。

---

## 8. 刷盘语义(P7M2-D10)

M2 沿用现有四个触发,全在 reactor 线程:

| 触发 | 发生点 |
|---|---|
| 缓冲攒满 | `dc_.wal.WriteWal` 内(`wal.cpp:560/568`),即 `ExecutePut`/`ExecuteDelete` 执行中 |
| Close | Stop op → `CloseDc` → `dc_.wal.Close`(刷净再关) |
| 收紧档 | `ExecuteSetWalLevel` 的 `dc_.wal.Flush()`(§6.1) |
| 快照前 | `DoSnapshot` 首步 `dc_.wal.Flush()`(自动触发或手动 Snapshot) |

**定时刷(`wal_flush_interval_ms`)继续延后**:旧引擎本就没接(`options.h:50`/`wal.cpp:567` 注明留 P7),
M2 不加 = 保行为;落地需把 `Run` 的无限期 `inbox_.wait` 改成带超时 + 超时刷,属后续轮。

**攒批档(2 ValueSync / 4 Async)语义**:Put 缓冲入帧即内存返回,下一触发前崩溃 → 缓冲帧丢、这几笔
Put 恢复不回来(级别 2 value 已 FUA 在盘但缺 WAL 映射成孤块;级别 4 二者皆可能丢)。这是选攒批档的既有取舍,
窗口由非定时触发兜底。**读己之写不受影响**:Put 返回后 value 已在盘、索引已在内存更新,Get 走"索引→块→读盘",
与 WAL 是否刷无关(`ReadYourWritesAllLevels` 四级别全过)。

---

## 9. 测试设计(P7M2-D11)

### 9.1 39 个写路径用例复活(零改写)

经冻结的公开 API(P7-D17),Put/Delete/Snapshot/SetWalLevel 接到 reactor 后,这些用例**透明地变成测 reactor
写路径**,测试代码一行不改——它们就是"reactor 行为等价于旧同步引擎"的最强回归护栏。

| 测试文件 | 复活用例数 | 备注 |
|---|---|---|
| `engine_test.cpp` | 10 | Put/Get/覆盖写/Delete/容量耗尽/CRC/校验码(`PutEmptyKeyFails`/`PutWrongValueSizeFails` 因校验上移转绿) |
| `wal_test.cpp` | 8 | 7 个 Put/Delete 驱动即绿;`FlushOnTighten` 经 D8 转绿(唯一多一步) |
| `snapshot_test.cpp` | 13 | Put + Snapshot 驱动(含阈值自动触发 / 双槽代际 / 恢复挑选) |
| `recovery_test.cpp` | 8 | Put/Delete/Snapshot + 崩溃恢复往返 |

> 直驱下层模块的用例(wal 27 / snapshot 4 / engine 8 已绿)本就不经 Engine 写 API、一直绿,M2 不影响。
> **无任何用例需结构性改写**(无多线程前提、无直接构造 reactor 状态;多线程测试是 M3 新增)。

### 9.2 reactor_test 新增(M2 补强)

1. **Put-Get 命中测**:Put 一值 → Get 回来 → 断言字节一致。**首次跑通 `ExecuteGet` 完整取数路径**(M1 只测
   Get-miss,`Lookup` 未命中即返回,从未走到 `pool→io.Read→CRC→memcpy→out`);也让"只跑 `--filter Reactor`"
   能自证读+写两路。
2. **写路径压测/活性测**:多轮 Put→Get→Delete 循环,断言每轮正确 + 不挂。镜像 M1 的 `RepeatedGetMiss`,是
   reactor_test 相对 engine_test 的独有价值——负载下压写交接(不丢唤醒、不死锁),**sync+TSAN 下是退出 ④ 最硬证据**。
3. **不在 reactor_test 放 recover 带数据**:`recovery_test` 已厚覆盖且 setup 重;M1 的 `RecoverEmptyThenGetMiss`
   已测过 recover→reactor 接线。

### 9.3 退出回归矩阵

| 构建 | 覆盖 |
|---|---|
| sync(Debug) | 全量(39 复活 + M1 reactor 用例 + 新增两项) |
| **sync + TSAN** | 同上,验写交接 race-free(退出 ④);TSAN 对全部二进制生效,写路径经 engine_test/recovery_test + 新压测都在 TSAN 下跑 |
| io_uring(Debug)+ ASAN | io_uring **数据路径首次经 reactor 被压**(M1 仅 Open/recover/Close;M2 的 Put 走 `io.Write`、Get 命中走 `io.Read`)。io_uring 与 TSAN 不兼容,race 检查归 sync(交接逻辑后端无关) |
| run-coverage | 报告含 `reactor.cpp` 写路径行覆盖 |

---

## 10. 退出条件

1. 单调用线程经 reactor 完成 Put/Get/Delete 全套,等价原同步引擎(engine_test 复活全过)
2. 覆盖写、Delete-then-Put、容量耗尽(`kEngineNoSpace`)、CRC 比对等现有 `engine_test` 场景全过
3. recover 后经 reactor 跑写路径 + 再 recover,数据一致(recovery_test 复活全过)
4. **写路径交接 race-free**(sync + TSAN;Put-Get 命中 + 写路径压测 + 复活用例均在 TSAN 下绿)
5. (观察)单线程 Put/Get/Delete p50,只记录、不卡门槛(P7-D2)
6. `FlushOnTighten` 经 SetWalLevel 收紧语义转绿
7. 全量在 sync(Debug)+ sync/TSAN + io_uring/ASAN 下回归全绿;覆盖率含写路径

---

## 11. 关键技术备忘

1. **栈节点逐轮复用 / 写 op 安全靠 last-touch 条款**:广播每轮新建栈 `OpNode`、reactor `Finalize` 在
   `result.store(release)` 之后只碰 WakeSlot.gen(进程级长寿槽,非 thread_local)、**不再碰 OpNode**,故调用方一读到
   result 即可销毁该栈节点。写 op 与读 op 同理,M2 不引入新同步面。
2. **内存序无新增**:写执行体跑在 reactor 线程,对 `dc_` 的全部改写经 `Finalize` 的 `result.store(release)`
   →调用方 `result.load(acquire)` 发布;全 release/acquire,无 seq_cst(同 M1 §4.6,group commit 的四件套
   M2 全程不碰)。
3. **per-reactor `wal_level` 无锁**:写(SetWalLevel op)与读(io/wal)都在属主 reactor 单线程、按 inbox 串行;
   靠"读写同线程"消 race,不是靠锁/原子(§4.4)。M4 上 N>1 时,各 reactor 各改各的拷贝、`Engine::SetWalLevel`
   广播保持全体齐步,亦无共享变量。
4. **share-nothing 是文件边界**:写路径全部迁入 `reactor.cpp`、以 `dc_` 为隐式对象,Engine 物理拿不到 dc;
   "这段代码 race-free 吗"靠"它在不在 reactor.cpp"回答(§11 D2)。
5. **行为严格保 P6**:Put/Delete/DoSnapshot/SetWalLevel 逐字照搬 P6M4,旧块回收时序、救援快照与 Insert 的
   先后、收紧只刷 WAL 不补 FUA——一律不"顺手改进",改进留性能轮(P7-D10/D13)。
6. **`MaybeRequestSnapshot` 内联代价**:触发快照的那次 Put 延迟含整份快照写(同线程同步)。简单版接受、与旧引擎
   一致;切出快照异步化属流水线债务(性能轮)。
