# Cabe P7-M3 设计:单设备多线程

> 本里程碑让**多个调用线程并发使用同一个 Engine**(N=1、单 reactor、多 caller)。与 M1/M2 不同,
> M3 **不新增并发机制**——reactor 的入站队列、唤醒、结果发布、串行执行从 M1 起就是按 MPSC(多生产者
> 单消费者)建的。M3 的本质是:**用多线程实测 + TSAN 多轮,把 M1/M2 的多生产者正确性钉死**,并厘清
> `Close` 的并发契约。它是一个**由 TSAN 证伪的假设**:全绿即证明机制一直正确;若 TSAN 抓出问题,那个
> 发现就是 M3 要修的(届时含一处机制修补)。
>
> **本文为详细设计**,grill 的全部裁决(A1–A3 直接采用 + D1–D5 逐条讨论)汇集于此。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P7 / M3 |
| 状态 | ✅ **已实装**(P7M5 收敛) |
| 上游依赖 | P7M1(reactor 异步机制)、P7M2(写路径入 reactor)全部完成;P7 阶段计划(doc/P7/README.md,P7-D1~D13);doc/P7/P7M2_write_path_design.md |
| 下游依赖本里程碑 | P7M4(多设备:真实路由 `hash%N` + N 个 reactor 并行 + 运营口真广播)→ M5(隔离 + 收敛) |
| 退出判定 | 见 §8 |

---

## 1. 目标与范围

### 1.1 目标

1. **多调用线程并发正确**:多个 caller 线程并发 Put/Get/Delete/SetWalLevel/Snapshot,所有 op 结果正确、
   无丢唤醒、无 data race(sync + TSAN 多轮)。
2. **运营 op 实证 + Close 契约**:SetWalLevel / Snapshot 与并发 Put 交错跑、在 reactor 串行下"竞态消失"的实证;Close 不与并发 Put 交错,其安全由 §4 静默契约(先 join 再 Close)保证,以 harness 的 join-then-Close 示范。
3. **Close 并发契约**:厘清并文档化 `Open`/`Close` 与并发 op 的关系(D2)。
4. **内存序复审**:在真多生产者下复核所有同步边仍全 release/acquire、无需 seq_cst(D5),并把多生产者
   可见性的依据(C++20 release-sequence)与硬约束写进注释。
5. **数据一致**:并发写后 recover,盘面/索引一致。

### 1.2 交付范围

1. **`test/engine/engine_concurrency_test.cpp`**(新建,M3 主体工作量):6 个并发用例(见 §7)。
2. **`test/CMakeLists.txt`**(修改):注册独立目标 `test_engine_concurrency`(仿 `test_reactor`:
   `add_executable` + 链 `cabe::engine` + `gtest_discover_tests`)。
3. **`engine.h`**(修改,**仅注释**):类级 + `Close` 声明处写明 D2 契约。
4. **`engine/reactor.h` / `reactor.cpp`**(修改,**仅注释**):在 `Submit`/`DrainAll` 处补一段
   release-sequence 依据 + "push 必须 CAS、drain 必须 acquire"的硬约束(§5)。
5. **`doc/P7/P7M3_single_device_mt_design.md`**(本文)。

**零改动 / 不动**:**reactor / engine 的并发机制逻辑零改动、零新增原子、零新增错误码**;`cabe_engine`
库源不变(无新增源文件)→ `engine/CMakeLists.txt` 不动;group commit(P7-D10)、盘上格式、恢复链逻辑不碰。

> 注:M3 是"零机制**逻辑**改动",但**新增测试 + 加若干注释**;"零改动"指不动并发机制的执行逻辑,不是不出交付物。

### 1.3 明确不做(各有归处)

| 不做项 | 归处 |
|---|---|
| 让 `Close` 与活跃 op 并发安全的机制(引用计数 / RCU) | **不做**,改用 D2 契约(同步 API 下静默即无并发;机制会污染热路径换没人要的场景) |
| 多设备 / 真实路由 `hash%N` / 运营口并行广播(N>1) | P7M4 |
| 流水线及其债(committed_seq / 同 key 乱序 / 跨线程 BufferPool / 模糊快照) | 性能轮(P7-D13;M3 无流水线,同 key 天然全序) |
| group commit 拆壳 / 真批量 fdatasync / RWF_DSYNC | 性能轮(P7-D10) |
| 显式背压 | 不做(P7-D9;同步 API + 栈驻节点 → 在途 op ≤ 线程数) |
| 钉核 / NUMA / spin-then-wait | 性能轮 |
| 多设备横向扩展吞吐 | M4(R=1 单 reactor 本就串行,M3 不指望扩展) |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 | grill 来源 |
|---|---|---|---|
| **A1** | 运营 op 并发安全 | SetWalLevel/Snapshot 多 caller 并发安全:Engine 侧只读 `reactors_` + 投 op,reactor 串行执行;多读者无写者(仅 Close 写) | A |
| **A2** | 背压 | 不做(P7-D9):同步 API + 栈驻节点 → 在途 op ≤ 线程数,inbox 涨不爆 | A |
| **A3** | group commit | 不碰(P7-D10):reactor 单写者直调现有 `WriteWal`;kWalFull 救援"重试恰一次必成"在 reactor 串行下仍成立 | A |
| **M3-D1** | 交付边界 | reactor 机制**零逻辑改动**(前提 D2=契约);M3 = 多线程测试 + TSAN 多轮 + 契约/依据注释 + 文档;是 TSAN 证伪的假设、非空保证 | D1 |
| **M3-D2** | Close 并发契约 | **契约**(非机制):Open/Close 排他,调用方保证其执行期间无其他 Engine 调用重叠;其余 op 彼此自由并发。同步 API 下"在途"⟺"caller 阻塞"⟺"未静默" | D2 |
| **M3-D3** | 测试形态 | 经 Engine **公开 API**(不开 friend、不直打 reactor);复用 `test_wal_concurrency` 的 barrier 组 + 自由跑 + join 活性;独立目标 `test_engine_concurrency`;harness 以"先 join 再 Close"示范契约 | D3 |
| **M3-D4** | 场景 + 规模 + 判定 | 6 用例覆盖①②③④;线程 `min(8,max(2,hw))`;廉价用例高量、真 I/O 用例 Put-Delete 回收控容量、多轮;判定 = 正确性 + join 活性 + TSAN 零报告 + 吞吐只记录 | D4 |
| **M3-D5** | 内存序 + 同 key 语义 | 真 MPSC 全 release/acquire、无 seq_cst(可见性靠 release-sequence);同 key 由 reactor 串行线性化、到达序最后写赢、无撕裂,读己之写仅对独占 key 成立;与 P7-D13 延后的流水线乱序无关 | D5 |

---

## 3. 为何"零机制改动"(M3-D1 的依据)

多调用线程需要的并发安全**已经全在当前代码里**,逐处审:

| 机制 | 位置 | 多调用线程下为何已安全 |
|---|---|---|
| 入队 push | `reactor.cpp:55` `Submit`(`compare_exchange_weak`,`reactor.cpp:59`) | Treiber 多生产者无锁 push:冲突即重载 `old` 重试;成功的 release 发布 op 内容(§5) |
| 整批摘 drain | `reactor.cpp:64` `DrainAll`(`exchange(nullptr, acquire)`,`:67`) | 单消费者;acquire 看到所有 push 的 release(release-sequence,§5) |
| 结果发布 + 唤醒 | `reactor.cpp:81` `Finalize` | 按每 op 的 `wake` 指针精确唤醒对应 caller;`result.store(release)` 为对 op 的最后一次访问(last-touch) |
| 空闲等待 | `reactor.cpp:106` `Run` / `:110` `inbox_.wait(nullptr, acquire)` | 原子 wait 自带"检查空与阻塞"原子;并发 push 的 notify 唤醒,无丢唤醒 |
| 调用线程等待 | `reactor.cpp:286` `SubmitAndWait` | 每 caller 用自己的进程级长寿 WakeSlot 槽(`CallerWakeWord` 分配,非 thread_local)、自己的栈 `OpNode`;投递后只碰自己的 WakeSlot.gen + 自己的 `op.result` |
| 数据 op 入口 | `Engine::Put/Get/Delete` | 各自栈上建 op、**只读** `reactors_[RouteKey]`、投递;caller 侧不碰 `dc_`(只有 reactor 线程碰),不写任何 Engine 共享态 → 天然可重入 |
| 运营 op 入口 | `Engine::SetWalLevel/Snapshot` | 只读 `reactors_` + 投 op;多 caller = 全是 `reactors_` 读者(A1) |
| 开关 flag | `Engine::opened_` | 单 flag,Open 万事就绪后 store(release),caller load(acquire);非 Dekker 对,无需 seq_cst |

这些覆盖了 M3 要并发砸的全部机制——它们**从 M1 设计就按 MPSC 建**(release/acquire 链、last-touch 条款、
单消费者无 leader 选举),不是 M3 临时补的。唯一可能逼出代码的点是 `Close` 与并发 op(`reactors_` 唯一
写者),由 **§4 契约**消除。

---

## 4. Close 并发契约(M3-D2)

### 4.1 竞态

`Engine::Close` 做 `opened_=false` → 逐 reactor `Stop()`(drain-then-close)→ `reactors_.clear()`。
`clear()` 是 `reactors_` 的**唯一写者**,而数据/运营 op 只读 `reactors_[i]`。二者真并发会撞两层:
1. **vector 数据竞争**:`clear()` 改 size/存储与 `reactors_[i]` 读并发(std::vector 非线程安全)= UB;
2. **Reactor 生命周期 UAF**:`clear()` 析构 `unique_ptr<Reactor>`,若某 caller 正握 `Reactor&` 在 Submit。

### 4.2 关键洞察:同步 API 下"在途"⟺"未静默"

同步 API 里,一个 op "在途" 当且仅当某 caller 线程**阻塞在 `SubmitAndWait`** 里——也就是它**还没从
`Put/Get/Delete` 返回**。所以只要契约是"Close 前所有数据 op 线程已返回",就**根本不存在并发**:没有
caller 在读 `reactors_`,vector 竞争和 UAF 都不发生。

### 4.3 两个选项与裁决

| | 契约(选定) | 无锁机制(否决) |
|---|---|---|
| 代价 | 零代码(一行注释)、零热路径开销 | `shared_ptr<Reactor>` + 原子引用计数 / RCU:**每个 Put 热路径多一个争用原子** |
| 换来 | 调用方在 Close 前协调关停(本就该做) | 支撑"Close 撞活跃请求"——**没人想要**的用法 |
| 对齐 | 无 mutex(D16/D18)、不污染热路径(P7-D2)、嵌入式引擎通行做法(RocksDB/LMDB) | 与 P7-D2 冲突 |

中间地带(in-flight 计数 + 自旋等静默)本质是重造引用计数,同样污染热路径,不取。

### 4.4 契约(精确表述)

> **`Open` 与 `Close` 是排他操作:调用方必须保证其执行期间没有任何其他 Engine 方法调用与之重叠**
> (通过 join 工作线程或等价同步建立 happens-before)。**数据/运营 op(Put/Get/Delete/SetWalLevel/
> Snapshot)彼此之间可自由并发**——只有 Open/Close 排他。

场景压测:8 工作线程循环 Put/Get,主线程要 Close。**正确**:主线程令工作线程停、`join`(所有 Put/Get
已返回),再 Close。**错误**:工作线程还在循环时主线程直接 Close = 契约违反 = UB。

### 4.5 附带点

- **`opened_.store(false)` 保留**为**尽力防呆**(良性交错下让"Close 后又 Put"拿到 `kEngineNotOpen`),
  **非**同步保证;真正安全来自契约。
- **drain-then-close 的 `FailWakeChain` 保留**为防御角落(P7-D11"reactor 停止必唤醒所有等待者";
  no-fault-injection),契约下不触发。
- **`~Engine` 自动 Close** 同受此契约覆盖——别在还有 op 在途时析构 Engine(C++ 对象生命周期常识)。
- **代码量**:零逻辑改动,仅 `engine.h` 类注释 + `Close` 声明处写明契约。

---

## 5. 内存序复审(M3-D5 上半)

真 MPSC 把**生产侧变成真并发**。逐条审同步边,看 M1 的"全 release/acquire、无 seq_cst"是否仍成立:

| 同步边 | 序 | 真 MPSC 下 |
|---|---|---|
| Submit 发布 op 内容 → DrainAll | push CAS **release** / exchange **acquire** | 够(关键,见下) |
| Finalize 发布 rc + `op.out` → caller 读 result | `result.store` **release** / `result.load` **acquire** | 够(单 reactor 写、单 caller 读该 op) |
| 唤醒 → caller wait | `wake fetch_add` **release** + notify / `wait` **acquire** + 双检 result | 够(该 WakeSlot.gen 是 caller 自己的进程级长寿槽,单写单读) |
| `opened_` Open 置位 → caller 读 | store **release** / load **acquire** | 够(单 flag,非 Dekker 对) |
| reactor 空闲 wait / push notify | wait **acquire** / notify | 够(原子 wait 自带重检,多 notify 幂等) |

### 5.1 关键:多生产者 push 的可见性(C++20 release-sequence)

唯一需要细想的是第一条——多个生产者并发 push,**单个** acquire-exchange 能否看到**所有**被推节点的内容?

能,靠 **C++20 的 release-sequence 规则**:
- 生产者 A 的 push 是 `compare_exchange`(一个 RMW)、release 写 `inbox_=opA`,**领起一条 release 序列**;
- 生产者 B 的 push 也是 RMW,读到 A 写的值(opA)→ B 的 CAS **并入 A 的 release 序列**(C++20 起,RMW
  不论自身内存序都算入序列,哪怕 B 读 `old` 用的是 relaxed);
- 消费者 `exchange(acquire)` 读到链尾(opB,B 写、属 A 的 release 序列)→ 消费者 acquire
  **synchronizes-with A 的 release** → A 在 push 前对 opA 各字段的普通写 happens-before 消费者读它们。

所以**每个被 push 的节点内容都对单消费者可见**,无需 seq_cst。这正是 Treiber 栈"release-CAS 入 +
acquire-exchange 整批摘"对 MPSC 正确的根因——M1 的选型从一开始就是按多生产者建的、不只单生产者。

> M1 设计稿"无 leader 选举 → 无 seq_cst"在多生产者下依然成立:P6 的四个 seq_cst 是为封 leader 选举那对
> 跨变量 Dekker store-load;reactor 没有选举,生产者之间不需互相看见(只需各自 push 进栈),消费者靠
> release-sequence 看见全部——全程 release/acquire 即足。

### 5.2 硬约束(写进注释,M3-D5 的真正交付)

release-sequence 依赖两点,谁动谁碎:
1. **push 必须保持 RMW(CAS)**——换成非 RMW 的 store-push,release 序列断链,多生产者下消费者看不到部分节点;
2. **drain 必须 acquire**——降成 relaxed 则不 synchronizes-with 任何 push。

M3 在 `Submit`/`DrainAll` 处补注释讲清此依据与约束。**M3 不新增任何原子**(零机制改动),审计结论:
**全 release/acquire 成立、无 seq_cst**。

---

## 6. 同 key 并发语义(M3-D5 下半)

并发对同一 key 操作的语义,把它钉死(决定 §7 同 key 用例怎么断言):

1. **M3 无流水线**(P7-D13)。reactor **一个 op 整跑完**(ExecutePut:写块→WriteWal→Insert 全做完)才下
   一个 → 同 key 的 op 被**到达序完全线性化**。
2. **到达序不确定**(哪个 caller 的 CAS 先赢是时序问题)→ 谁是"最后一次 Put"不确定 → 最终 Get 读到谁的值
   不确定。**到达序最后写赢**。
3. **每个 op 对其他 op 原子**(reactor 串行)→ 并发 Get **绝不读到撕裂/半写值**,只读到"reactor 序里最近
   完成的 Put"的完整值,CRC 必对。覆盖写下:ExecutePut 写**新**块 → Insert 指新块 → 回收**旧**块;串行下
   后续 Get 要么旧索引→旧块(整写过)、要么新索引→新块(整写过),旧块回收后即便被未来 Put 重用也是被
   串行的未来 op——**没有 Get 读到正被并发写的块**。
4. **读己之写仅对独占 key 成立**:线程独占的 key(§7 不相交用例)Put 同步返回后 Get 必读回自己的值;但
   **争用 key 上读己之写不成立**——A 同步 Put 返回后再 Get,中间 B 的 Put 可能已被处理,A 合法读到 B 的值。

### 6.1 与延后债务划清界限

P7-D13 延后的"同 key 乱序"是**流水线**产物——多 op 在飞、异步 I/O 时同 key 可能乱序完成,需 committed_seq
兜。M3 **没有流水线**,同 key 天然全序、不需 committed_seq、不可能乱序。所以 M3 不碰那笔债,它在这里
**根本不存在**;M3 的同 key 是"被串行干净线性化"的简单情形,可测且正确。

### 6.2 测试断言原则

争用 key 上断言**自洽**而非确定结果:Get 返 `kSuccess`、值**字节完整**且等于 N 个写入值之一(每线程写
可辨认模式如全 `byte{tid}`)→ 断言均匀字节 `b` 且 `b ∈ 合法 tid`、CRC 对;**不**断言特定 `b`、**不**假设
读到自己的值。

---

## 7. 测试设计(M3-D3 + M3-D4)

独立目标 `test/engine/engine_concurrency_test.cpp`,**经 Engine 公开 API**,复用 `test_wal_concurrency`
的 harness(barrier 组对齐投递最大化 push 争用 + 自由跑随机交错 + join 全部线程的活性判定)。每个用例骨架
**"Open → 起 N 工作线程跑 op → join 全部 → Close"**,即以"先 join 再 Close"示范 §4 契约。

### 7.1 六个用例

| 用例 | 砸法 | 覆盖 | 断言 |
|---|---|---|---|
| **ConcurrentGetMissStorm** | N 线程各狂跑 Get-miss(不存在的 key),barrier + 自由跑 | ① | 每个 Get 返 `kIndexKeyNotFound`;全 join(纯交接压测、无 I/O,最大化 CAS 入栈 + 唤醒边) |
| **ConcurrentDisjointRoundTrip** | N 线程各占**不相交** key 空间,循环 Put→Get→Delete→Get | ①③ | 各线程读回**自己**的值(独占 key 读己之写成立)、Delete 后 miss;全 join |
| **SetWalLevelDuringPut** | N-1 线程 Put/Get/Delete 不相交 key,1 线程循环 `SetWalLevel` 轮换四档 | ②(SetWalLevel‖Put) | op 返码合法、数据正确、SetWalLevel 返 ok、不崩、全 join |
| **SnapshotDuringPut** | N-1 线程 Put/Delete,1 线程循环手动 `Snapshot()` + 小 `snapshot_threshold_bytes` 让自动快照也插进来 | ②(Snapshot‖Put) | op 合法、Snapshot 返 ok、数据自洽、全 join |
| **ConcurrentSameKeyNoTear** | N 线程全砸**同一 key**,各写全 `byte{tid}` 填充值,穿插 Get | ① + 同 key 边界 | Get 读到完整值且 ∈ 合法 tid 集、CRC 对(自洽非确定,§6.2) |
| **ConcurrentWritesRecoverConsistent** | N 线程并发 Put(无 Delete)不相交 key 到**已知终态**(全部 key 应存),join → Close → 重开 recover | ③ | 每个写入 key recover 后读回应有值 |

吞吐(④)不单列:在某用例记一条 ops/s **日志**,不断言(P7-D2;R=1 串行预期≈单线程,不指望扩展);
细测走 `run-bench.sh`。

### 7.2 规模

- **线程数**:`min(8u, max(2u, std::thread::hardware_concurrency()))`——≥2 才有争用,封顶 8 免 TSAN 太慢。
- **廉价用例**(GetMissStorm):线程 × 数千次(无 I/O,TSAN 下也快)。
- **真 I/O 用例**:线程 × 数十~一两百 op;**用 Put→Delete 循环回收块**,任意时刻在生块 ≤ 线程数。
- **recover 用例**:终态在生 key 数适配数据盘容量(loop 盘小;远小于 `PutUntilFull` 上限)。
- **多轮**:用例内不做整场景重复轮(每个用例只跑一次并发突发);多轮覆盖靠手动多跑二进制(README"sync+TSAN 多轮")。线程体内的 for 循环是「量级」(GetMiss 1000、真 I/O 用例 80/100、recover per≈2~3),非把并发场景重复 2~3 轮。

### 7.3 判定

1. **正确性**:逐 op 返码合法;不相交 key 读回自己的值;同 key 自洽;recover 终态精确。
2. **活性**:全线程 join——死锁/丢唤醒 = ctest 超时(唯一可靠判定,非误绿)。
3. **无竞争**:二进制在 **sync + TSAN** 下零 ThreadSanitizer 报告(M3 硬核证据)。
4. **吞吐**:只记日志,不断言。

### 7.4 测试设计约束

数据盘容量有限:高量级用例**必须靠 Put→Delete 回收**控在生块数;recover 用例终态要**实测 < 盘容量**
(否则撞 `kEngineNoSpace` = 容量问题非并发问题)。**io_uring 后端用 ASAN/UBSAN**(io_uring 与 TSAN 不
兼容,SQ/CQ 内核共享内存会误报);race 检查统一归 sync(交接逻辑后端无关)。

---

## 8. 退出条件

1. 多调用线程并发砸单 reactor,所有 op 正确、**无丢唤醒、无 race**(sync + TSAN 多轮)
2. SetWalLevel/Snapshot 与并发 Put 交错、reactor 串行下结果一致不崩的实证(SetWalLevelDuringPut / SnapshotDuringPut);Close 由 §4 静默契约(join 后再 Close)保证安全,以 harness 的 join-then-Close 示范,不与并发 Put 交错
3. 多线程下数据一致(并发 Put/Get/Delete + recover,盘面/索引一致)
4. (观察)多 caller 单 reactor 吞吐——R=1 单 reactor 串行,吞吐≈单线程,**不指望扩展**(扩展是 M4 加设备)
5. 全量回归仍全绿(sync + io_uring);新目标 `test_engine_concurrency` 在 **sync + TSAN** 下绿、io_uring + ASAN 下绿
6. 内存序复审结论落注释:全 release/acquire、无 seq_cst,release-sequence 依据 + push-CAS/drain-acquire 约束

---

## 9. 关键技术备忘

1. **M3 不造机制,只证机制**:reactor 从 M1 起就是 MPSC(CAS-push、per-caller wake、per-op result、单消费者
   串行);M3 用并发实测把这份前瞻性钉死。"零机制改动"是有强论据的假设,TSAN 多轮是它的证伪手段。
2. **多生产者可见性 = release-sequence**:CAS-push 是 RMW、链入前一 push 的 release 序列,单 acquire-drain
   即见全部节点内容。**硬约束**:push 保 CAS、drain 保 acquire(§5.2)。这是 M3 最该懂、也最该写进注释的点。
3. **Close 靠契约不靠锁**:同步 API 下"在途 op"⟺"caller 未返回",静默契约即零并发;引用计数会拿热路径
   代价换没人要的"Close 撞活跃请求"。Open/Close 排他,其余 op 彼此自由并发。
4. **同 key 无流水线即无乱序**:reactor 一个 op 整跑完才下一个 → 同 key 到达序全序、无撕裂、无需
   committed_seq;P7-D13 延后的乱序是流水线的事,M3 不存在。读己之写仅对独占 key 成立。
5. **活性靠超时兜底**:并发死锁/丢唤醒不会"假绿",会挂在 join 上 → ctest 超时(同 P6 `test_wal_concurrency`)。
6. **吞吐不卡门槛**:R=1 单 reactor 串行,多 caller 吞吐≈单线程是**预期**而非缺陷;横向扩展是 M4 加设备的事
   (P7-D2/D7)。
