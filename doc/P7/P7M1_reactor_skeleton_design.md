# Cabe P7-M1 设计:reactor 机制(读路径)

> 本里程碑建起 P7 的 reactor 异步机制——把数据通路从调用线程搬进 per-reactor 事件循环,
> 用最简的读路径(Get)端到端验证。调用线程投递 op 后挂起,reactor 单线程处理后唤醒;
> 全程无 mutex。**M1 只做 Get + Stop**,写路径(Put/Delete)与运营口留 M2;**M1 测试只验
> 异步机制**(create 空设备 → Get 必 miss → Close + TSAN),取值正确性与恢复随 M2 的 Put-Get
> 联动测。
>
> **本文为详细设计**,七个议题(C1 交付边界 / C2 接口与 OpNode / C3 入站队列 / C4 同步包异步 /
> C5 生命周期 / C6 Get 执行体 / C7 Engine 改造与测试)的全部裁决汇集于此。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P7 / M1 |
| 状态 | ⏳ **设计中(待实装)** |
| 上游依赖 | P6 全部完成;P7 阶段计划(doc/P7/README.md,P7-D1~D13) |
| 下游依赖本里程碑 | P7M2(写路径入 reactor:Put/Delete 执行体 + 运营口 + per-reactor Options + Get 取值/recover 联动测)→ M3 → M4 → M5 |
| 退出判定 | 见 §9 |

---

## 1. 目标与范围

### 1.1 目标

1. **reactor 异步机制实装**:`Reactor` 类 + 入站 MPSC 队列 + 工作线程 + 事件循环;同步包异步
   (调用线程投递 → 挂起 → reactor 处理 → 唤醒)。
2. **读路径端到端**:Get 经 reactor 执行;调用线程拿到正确结果码。
3. **所有权交接**:`DeviceContext` 由 reactor 独占持有(P7-D4);Engine 退化为持 reactor 句柄 +
   路由 + 同步包异步门面。
4. **干净启停**:Open 起 reactor、Close 走 drain-then-close + join,无等待者孤儿、无 fd 泄漏。
5. **测试只验机制**:create 空设备 → Get 必 miss → 走完异步一圈 + Close + TSAN;不碰伪造、不需 Put。

### 1.2 交付范围

1. **`engine/reactor.h`**(新建):`OpType` / `OpNode` / `kOpPending` / `Reactor` 类(接口 + 私有
   入站队列、`std::thread`、`DeviceContext dc_`、执行体声明)。
2. **`engine/reactor.cpp`**(新建,主战场):`Run` 事件循环、`Submit`、`Start`/`Stop`、`ExecuteGet`、
   finalize、入站 Treiber 栈(push/drain/reverse)、drain-then-close、调用线程侧 `thread_local`
   唤醒字 + 等待循环。
3. **`engine/engine.h`**(修改):`devices_` → `reactors_`(`vector<unique_ptr<Reactor>>`);
   `opened_` → `atomic<bool>`;include `reactor.h`;`RouteKey` 返扁平 reactor 下标。恢复方法签名不变。
4. **`engine/engine.cpp`**(修改):`Open` 两阶段;`Get` → 验证 + 路由 + submit + wait + 翻译;
   `Close` → 投 Stop + join + 聚合;`Put`/`Delete`/`SetWalLevel`/`Snapshot` → `kEngineNotImplemented`。
5. **`engine/CMakeLists.txt`**(修改):`reactor.cpp` 加入 `cabe_engine` 源。
6. **`test/engine/reactor_test.cpp`**(新建):机制测试(见 §8)。
7. **`test/CMakeLists.txt`**(修改):注册测试目标。

**零触碰区**:`wal/`(group commit 全程不碰,P7-D10)、`snapshot/`、`io/`、`index/`、`slots/`、
恢复链逻辑、盘上格式、错误码表(零新增——`kOpPending` 是 reactor 内部哨兵不进表,`kEngineNotImplemented`
已存在 -104004)。

### 1.3 明确不做(各有归处)

| 不做项 | 归处 |
|---|---|
| Put/Delete 写执行体 | P7M2 |
| SetWalLevel / Snapshot 运营口实装 | P7M2 |
| per-reactor Options 副本(消 SetWalLevel 跨线程 race) | P7M2(B6) |
| Get 取值正确性测试 + recover 带数据后 Get | P7M2(Put-Get 联动测) |
| group commit 简化 / 拆壳 | 性能轮(P7-D10;M1 全程不碰,reactor 单写者直调现有 `WriteWal`) |
| 多调用线程并发 | P7M3 |
| 多设备 / 路由真实化 / 运营口 fan-out | P7M4 |
| 钉核 / NUMA / spin-then-wait / 流水线 | 性能轮(P7-D2/D13) |
| 运行时 I/O 故障注入 | no-fault-injection(保留防御返回、不测) |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 |
|---|---|---|
| **P7M1-D1** | 交付边界 | reactor 只做 **Get + Stop**;`Put`/`Delete`/`SetWalLevel`/`Snapshot` 返 `kEngineNotImplemented`;测试只验机制(create 空 + Get miss + Close + TSAN),**取值正确性 + recover 带数据 + Put 随 M2 的 Put-Get 联动测**(不碰伪造 fixture) |
| **P7M1-D2** | Reactor 接口 + OpNode + OpType | `Reactor(DeviceContext&&)` move 接管、`Start()→int32_t` / `Stop()` / `Submit(OpNode*)`(**只投递不等待**);`OpNode` 建调用线程栈上、零堆分配:`{type, key(视图), out(DataBuffer), result{kOpPending}, next, wake}`;`OpType` M1 只 `Get`/`Stop`;**等待分到调用线程侧**(M4 fan-out 前向兼容);Reactor 不可移动 → Engine 用 `unique_ptr` 持有 |
| **P7M1-D3** | 入站队列 | 给 reactor **新写一份最小侵入式 Treiber 栈,inline 在 reactor.cpp**:复刻 P6 的 push / exchange-drain / reverse,**去掉 leader 选举与 epoch**(reactor 是唯一固定消费者);**保留 reverse**(到达序/drain-then-close 需要);否决抽公共原语(B8 不碰 WAL CommitGroup,共享在 P7 只一个用户)、否决直接复用 WAL 的栈 |
| **P7M1-D4** | 同步包异步 | **唤醒挂点(每调用线程一个 `thread_local wake_gen`)与结果槽(`OpNode.result`)分离**(避"最后触碰"UB);调用线程快照 gen **在 submit 之前**、`wake_gen.wait(gen)` 阻塞、`result` 作完成判据;reactor finalize 扣 **last-touch**(先存 next/wake → `result.store(release)` → wake `fetch_add`+`notify_one`);reactor 空闲在栈顶 `atomic::wait(nullptr)`;**全 release/acquire、不需要 seq_cst**(无选举的 Dekker 对);Get 的 `memcpy→op.out` 经 result 的 release/acquire 发布 |
| **P7M1-D5** | 生命周期 | `Start` 起线程(`std::thread` 构造 try/catch 转错误码——明显故障简单判断、不让异常逃逸);**Stop op 走 drain-then-close**(处理完 Stop 前的 op → close 序列 snapshot→wal→io 首错存 `close_result_` → 唤醒掉队 op 成 `kEngineNotOpen` → 退出 Run);`Close` = 逐 reactor 投 Stop + `join` + 聚合首错(**join 即同步,Stop op 无需 wake**);**Open 两阶段**(逐设备 recover 进临时 holder → 全成功后 wrap 进 Reactor + Start);Reactor 析构 RAII 兜底防 fd 泄漏 |
| **P7M1-D6** | Get 执行体 + 输出 buffer | 验证(`opened_`/`key.empty`/`value.size`)**上移调用线程 fail-fast**;执行体 = 现有 Get 逻辑原样搬进 reactor(改 dc 来源 + 输出到 `op.out` + 去验证);**保留 pool-buffer + CRC + 成功才 memcpy**(O_DIRECT 对齐 + CRC 失败不脏写 caller buffer);**pool 是 reactor 私有、单线程无锁**;完整执行体 M1 写全,但只测到 Lookup miss |
| **P7M1-D7** | Engine 改造 + 测试 | `devices_`→`reactors_`;`opened_`→`atomic<bool>`;Open 两阶段;Get→submit/wait;Close→Stop/join;其余公开方法 not-implemented;**options_ 留 Engine 持有、M1 只读安全**(无 SetWalLevel,per-reactor 副本是 M2);测试 = create 空设备 + Get-miss + Close + (可选)recover 空 + not-implemented + 坏 Open + TSAN + liveness,全走公开 API |

---

## 3. 核心结构

### 3.1 Reactor 类(`engine/reactor.h`)

```cpp
enum class OpType : std::uint8_t { Get = 1, Stop = 2 };   // M2 加 Put/Delete/SetWalLevel/Snapshot

inline constexpr std::int32_t kOpPending = 1;             // 正数哨兵,和 ≤0 错误码不撞;不进 error_code.h

struct OpNode {
    OpType type;
    std::string_view key;                       // Get 输入(caller 的 key,视图不拷贝)
    DataBuffer out;                             // Get 输出(caller 的 buffer)
    std::atomic<std::int32_t> result{kOpPending};
    OpNode* next = nullptr;                     // MPSC 侵入式链
    std::atomic<std::uint32_t>* wake = nullptr; // 指向 caller 的 thread_local 唤醒字
};

class Reactor {
public:
    explicit Reactor(DeviceContext&& dc);
    ~Reactor();                                 // RAII:线程已 join 则只关 dc(防双关)
    Reactor(const Reactor&) = delete;           // 不可拷贝
    Reactor& operator=(const Reactor&) = delete;
    Reactor(Reactor&&) = delete;                // 不可移动(线程引用 this)→ Engine 用 unique_ptr
    Reactor& operator=(Reactor&&) = delete;

    std::int32_t Start();                       // 起线程跑 Run();线程创建失败返错误码
    std::int32_t Stop();                        // 投 Stop op + join + 返回 close_result_
    void Submit(OpNode* op);                    // 入队 + 唤醒 reactor(不等待)

private:
    void Run();                                 // 事件循环
    std::int32_t ExecuteGet(OpNode* op);        // §6
    // 入站 Treiber 栈
    OpNode* DrainAll();                         // exchange(nullptr, acquire)
    static OpNode* Reverse(OpNode* head);       // LIFO → 到达序

    std::atomic<OpNode*> inbox_{nullptr};       // 栈顶,兼 reactor 空闲时的 futex 字
    std::thread thread_;
    DeviceContext dc_;
    std::int32_t close_result_ = err::kSuccess; // Run 退出前写,Stop join 后读
    bool dc_closed_ = false;                    // RAII 防双关
};
```

`PushWriter` 等价的入队由 `Submit` 实现(见 §4.4)。`CommitGroup` 那套三原子 + 选举 + epoch
**不出现**——reactor 是唯一固定消费者,只需一个 `inbox_`。

### 3.2 调用线程侧唤醒字

```cpp
// reactor.cpp 文件作用域:每调用线程一个,跨多个 reactor 共享(为 M4 fan-out 准备)
thread_local std::atomic<std::uint32_t> g_wake_gen{0};
```

挂点放 `thread_local`(生命周期 ≥ 线程),**不放 OpNode**——这是避"最后触碰"UB 的根本(§4.1)。

---

## 4. 同步包异步机制(核心)

### 4.1 核心安全:挂点与结果槽分离

reactor 对 OpNode 的**最后一次访问必须是 `result.store`**——这之后调用线程随时可能读到结果、
返回、栈上的 OpNode 析构。所以唤醒(`notify`)绝不能打在 OpNode 上,要打在比节点活得久的字上
= `g_wake_gen`(thread_local)。OpNode 只带一个指向它的指针 `wake`。

### 4.2 调用线程侧等待(在 `Engine::Get` 里)

```cpp
OpNode op;
op.type = OpType::Get; op.key = key; op.out = value;
op.result.store(kOpPending, std::memory_order_relaxed);
op.wake = &g_wake_gen;

std::uint32_t gen = g_wake_gen.load(std::memory_order_acquire);   // 快照必须在 submit 之前
reactor.Submit(&op);
while (op.result.load(std::memory_order_acquire) == kOpPending) {
    g_wake_gen.wait(gen, std::memory_order_acquire);              // wake_gen==gen 时阻塞
    gen = g_wake_gen.load(std::memory_order_acquire);
}
std::int32_t rc = op.result.load(std::memory_order_acquire);
return rc == err::kSuccess ? Status::Ok() : Status::Error(rc);
```

快照取在 submit 之前:reactor 在快照之后任何时刻 bump 了 `wake_gen`,`wait(gen)` 都会立即返回 →
不丢唤醒。这是 P6 `WaitResult` 同款。

### 4.3 reactor finalize(扣 last-touch)

```cpp
OpNode* next = op->next;                 // 先存 next(存结果后节点可能就没了)
auto*   w    = op->wake;                  // 先存 wake 指针
std::int32_t rc = ExecuteGet(op);         // 写满 op->out
op->result.store(rc, std::memory_order_release);   // 对 op 的最后一次访问
w->fetch_add(1, std::memory_order_release);
w->notify_one();                          // 精确唤醒该调用线程
// 推进到 next(见 §4.5 Run)
```

`next`/`wake` 必须在 `result.store` **之前**读进本地;store 之后只碰本地 `w`(指向仍存活的
thread_local 字)和 `next`,绝不再碰 op。

### 4.4 投递与 reactor 空闲等待

```cpp
void Reactor::Submit(OpNode* op) {                       // 调用线程
    OpNode* old = inbox_.load(std::memory_order_relaxed);
    do { op->next = old; }
    while (!inbox_.compare_exchange_weak(
        old, op, std::memory_order_release, std::memory_order_relaxed));
    inbox_.notify_one();                                 // 唤醒 reactor(唯一等待者)
}
```

reactor 空闲时在 `inbox_` 栈顶 `wait(nullptr)`(见 §4.5)。`atomic::wait(nullptr)` 自身保证
"检查空与阻塞"原子——push 若在检查后发生,wait 立即返回,无丢唤醒。

### 4.5 Run 事件循环

```cpp
void Reactor::Run() {
    while (true) {
        OpNode* batch = inbox_.exchange(nullptr, std::memory_order_acquire);  // 整批摘
        if (!batch) { inbox_.wait(nullptr, std::memory_order_acquire); continue; }
        for (OpNode* op = Reverse(batch); op; ) {        // 到达序
            OpNode* next = op->next;
            if (op->type == OpType::Stop) { HandleStop(op, next); return; }   // §5.2
            auto* w = op->wake;
            std::int32_t rc = ExecuteGet(op);
            op->result.store(rc, std::memory_order_release);
            w->fetch_add(1, std::memory_order_release);
            w->notify_one();
            op = next;
        }
    }
}
```

### 4.6 内存序总览(全 release/acquire,无 seq_cst)

| 操作 | 序 |
|---|---|
| `Submit` push CAS(发布 op 内容) | release |
| reactor `exchange`-drain / `inbox_.wait` | acquire |
| reactor `result.store`(发布执行结果 + `op.out`) | release |
| caller `result.load` / `wake_gen.wait`/`load` | acquire |
| reactor `wake_gen.fetch_add` | release |

> **为什么不需要 seq_cst**:P6 的四个 seq_cst 是为封 leader 选举那对跨变量 Dekker store-load;
> M1 reactor 没有选举,也就没有那对 store-load,全部 release/acquire + `atomic::wait/notify` 即足。
> 这是 M1 异步机制比 P6 group commit 简单的根本。

---

## 5. 生命周期

### 5.1 Start

```cpp
std::int32_t Reactor::Start() {
    try { thread_ = std::thread([this] { Run(); }); }
    catch (...) { return err::kEngineReactorStartFailed; }   // 明显故障,简单判断,不让异常逃逸
    return err::kSuccess;
}
```

> 注:`kEngineReactorStartFailed` 若 error_code.h 无现成贴切码,实装时在 engine 段追加一个;
> 它是"明显故障(线程/资源创建失败)"那一档(no-fault-injection),不做注入测试。

### 5.2 Stop op 的 drain-then-close

```cpp
void Reactor::HandleStop(OpNode* /*stop*/, OpNode* post_stop) {
    close_result_ = dc_.snapshot.Close();                    // 沿用现有顺序 snapshot→wal→io
    std::int32_t w = dc_.wal.Close(), i = dc_.io.Close();
    if (close_result_ == err::kSuccess) close_result_ = w ? w : i;
    dc_closed_ = true;
    // 唤醒掉队 op:本批 Stop 之后的链 + 再 drain 一次 inbox,全部回 kEngineNotOpen(不留孤儿)
    FailWakeChain(post_stop);
    FailWakeChain(Reverse(inbox_.exchange(nullptr, std::memory_order_acquire)));
    // return 后线程结束
}
```

`FailWakeChain` 对每个 op 走 §4.3 的 finalize、但 `rc = kEngineNotOpen`。最后一纳秒(最终 drain
之后才 push)的极限竞态是防御角落——`opened_=false`(§5.3)已挡掉合法调用方。

### 5.3 Close(Engine 侧)

```cpp
Status Engine::Close() {
    if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
    opened_.store(false, std::memory_order_release);         // 尽力挡新调用
    std::int32_t first = err::kSuccess;
    for (auto& r : reactors_) { std::int32_t rc = r->Stop(); if (first == err::kSuccess) first = rc; }
    reactors_.clear();
    return first ? Status::Error(first) : Status::Ok();
}
```

`Reactor::Stop()` = 在 Engine 线程栈上建一个 `OpNode{type=Stop}` → `Submit(&stop)` →
`thread_.join()` → `return close_result_`。**join 是同步点,Stop op 无需 wake**;Stop() 阻塞在 join
上直到 reactor 退出,期间 Stop op 一直活在 Stop() 栈帧里。

### 5.4 Open 两阶段

```cpp
// 阶段一:逐设备 recover 进临时 holder(无 reactor)
std::vector<DeviceContext> recovered;
for (i : devices) {
    DeviceContext dc;
    rc = create ? CreateDeviceGroup(...) : RecoverDeviceGroup(...);   // ① 超级块
    ... 打开 io、Init 分配器、create 开 wal/snapshot 或 RecoverDevice(...) ...
    if (rc) { AbortOpen(dc); /* 清 recovered 里的裸 dc */ return Error(rc); }
    recovered.push_back(std::move(dc));
}
// 阶段二:全成功后 wrap 进 Reactor + Start
for (auto& dc : recovered) {
    auto r = std::make_unique<Reactor>(std::move(dc));
    if (std::int32_t rc = r->Start()) { /* 停已起的 reactor + 析构未起的 */ return Error(rc); }
    reactors_.push_back(std::move(r));
}
opened_.store(true, std::memory_order_release);
```

recover 阶段失败时**没有 reactor 要停**(最简失败路径);只有全 recover 成功才起 reactor。
`RecoverDevice` 等恢复方法签名不变,作用在阶段一的裸 dc 上。

### 5.5 Reactor 析构(RAII 兜底)

正常流程 `Stop()` 已 join、`dc_closed_=true`。若 Start 失败/线程没起,析构里若 `!dc_closed_`
则关一次 dc(snapshot→wal→io),保证任何路径下 fd 不泄漏。

---

## 6. Get 执行体 + 输出 buffer

### 6.1 验证上移调用线程(`Engine::Get`,投递之前)

```cpp
Status Engine::Get(std::string_view key, DataBuffer value) {
    if (!opened_.load(std::memory_order_acquire)) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);
    Reactor& r = *reactors_[RouteKey(key)];     // M1:RouteKey 返 0
    // 建 OpNode + submit + wait + 翻译(§4.2)
}
```

### 6.2 执行体(reactor 线程,作用在 `dc_`)

```cpp
std::int32_t Reactor::ExecuteGet(OpNode* op) {
    ValueMeta meta{};
    std::int32_t rc = dc_.meta_index.Lookup(op->key, &meta);
    if (rc != err::kSuccess) return rc;                          // miss → kIndexKeyNotFound(M1 测的就是这条)
    std::byte* buf = dc_.pool.Allocate();
    if (!buf) return err::kEnginePoolExhausted;
    rc = dc_.io.Read(meta.block.block_idx(), buf);
    if (rc != err::kSuccess) { dc_.pool.Free(buf); return rc; }
    if (util::CRC32(DataView{buf, kValueSize}) != meta.crc) { dc_.pool.Free(buf); return err::kEngineDataCorrupted; }
    std::memcpy(op->out.data(), buf, kValueSize);
    dc_.pool.Free(buf);
    return err::kSuccess;
}
```

逻辑与现有 `Engine::Get`(engine.cpp:188-210)一致,只改三处:dc 来自 reactor 自己、输出写
`op->out`、验证已上移。**完整执行体 M1 写全,但只测到 Lookup miss**(命中路径随 M2 Put-Get 联动测)。

### 6.3 输出 buffer

caller 同步阻塞 → DataBuffer 全程有效;`memcpy → op->out` 经 `result` 的 release/acquire 对 caller
可见(§4.6)。保留 pool-buffer:① O_DIRECT 要 4K 对齐(pool buffer 对齐,caller buffer 不保证);
② CRC 失败时 caller buffer 不被脏写(成功才拷)。pool 是 reactor 私有、单线程无锁。

---

## 7. Engine 层改造

- `engine.h`:`std::vector<DeviceContext> devices_` → `std::vector<std::unique_ptr<Reactor>> reactors_`;
  `bool opened_` → `std::atomic<bool> opened_`;include `engine/reactor.h`;`RouteKey` 返扁平下标
  (M1 返 0)。恢复私有方法签名不变(作用阶段一裸 dc)。
- `engine.cpp`:`Open` 两阶段(§5.4);`Get` → 验证 + 路由 + submit + wait(§4.2/§6.1);
  `Close` → 投 Stop + join + 聚合(§5.3);`Put`/`Delete`/`SetWalLevel`/`Snapshot` → `opened_` 检查后
  返 `kEngineNotImplemented`;**options_ 留 Engine 持有**(M1 无 SetWalLevel、Open 后只读,reactor 里
  组件持 `&options_` 只读无 race;per-reactor 副本是 M2/B6);写路径内部(`MaybeRequestSnapshot`/
  `WriteWalRescuing`/`DoSnapshot` 等)M1 不碰、不用。
- `engine/CMakeLists.txt`:`reactor.cpp` 加进 `cabe_engine`。

---

## 8. 测试设计(`test/engine/reactor_test.cpp`)

全走公开 API,**无伪造、无 Put**。设备用 mkloop loop 三件套(`CABE_TEST_*`)。

| 用例 | 内容 | 验 |
|---|---|---|
| GetMissThroughReactor | create 空设备 → Get 任意 key | 返 `kIndexKeyNotFound`;整个异步机制走全(投递/事件循环/执行体/finalize/唤醒/等待),零数据 |
| CloseDrainThenClose | Open → 若干 Get(miss)→ Close | 干净退出、不挂、无 fd 泄漏 |
| RecoverEmptyThenGetMiss(可选) | create → Close → recover → Get(miss) | Open 两阶段对 recover 模式也成立(带数据 + 取值正确是 M2) |
| NotImplemented | Put/Delete/SetWalLevel/Snapshot | 返 `kEngineNotImplemented` |
| BadOpen | empty devices / size>1 | 返 `kEngineInvalidOpts` |

- **TSAN(sync 后端)**:跑上述,验 caller↔reactor 交接 race-free——单 caller 也是 caller+reactor
  两线程,TSAN 查得到 inbox CAS / futex / 结果槽 / 唤醒。
- **liveness**:测试不能挂——死锁/丢唤醒表现为 ctest 超时(同 P6 并发测试的判法),不是假绿。

---

## 9. 退出条件

1. Get(miss)经 reactor 返回 `kIndexKeyNotFound`,异步机制走全(GetMissThroughReactor ✅)
2. Close drain-then-close 干净退出,无等待者孤儿、无 fd 泄漏(CloseDrainThenClose ✅)
3. caller↔reactor 交接 race-free(sync + TSAN ✅)
4. liveness:不挂(ctest 不超时)
5. `Put`/`Delete`/`SetWalLevel`/`Snapshot` 返 `kEngineNotImplemented`;坏 Open 返 `kEngineInvalidOpts`
6. (观察)单线程 Get(miss)p50,只记录、不卡门槛(P7-D2)
7. 单线程现有非数据通路测试零退步;四档检测器 + io_uring(release)回归全绿

> **取值正确性、recover 带数据后 Get、Put/Delete 不在 M1 退出条件**——随 M2 的 Put-Get 联动测。

---

## 10. 关键技术备忘

1. **挂点必须比 op 节点活得久**:reactor 在 `result.store` 之后 `notify`,而 caller 可能已读结果、
   返回、栈帧(含 OpNode)析构——所以挂点放 `thread_local g_wake_gen`、不放 per-op 栈节点
   (迁移 P6"最后触碰条款")。
2. **比 P6 简单**:reactor 是 WAL/inbox 的唯一消费者,无 leader 选举,故无 seq_cst 四件套,
   全 release/acquire。group commit 的并发机械 M1 全程不碰(reactor 单写者直调现有 `WriteWal`、
   自任 leader 批 1),它在 P7 是死代码、留着无害(P7-D10)。
3. **share-nothing 是类型保证**:Reactor 独占持有 `dc_`,Engine 物理拿不到、调用线程更碰不到,
   一切访问走 `Submit`——这是 io_uring 不能跑 TSAN 时唯一可靠的越界防线。
4. **Reverse 不可省**:reactor 必须按到达序处理,否则最后投进来的 Stop op 会排到栈顶被最先处理,
   破坏 drain-then-close。
5. **pool 在 M1 天然无锁**:reactor 独占 dc → pool 单线程用。流水线下才会变跨线程,那是性能轮的事。
