# Cabe P7-M4 设计:多设备

> 本里程碑让引擎跨 **N≥2 个设备**工作:每设备一个 reactor、按 key 的 hash 路由、跨设备零通信、R=1。
> 与 M1–M3 不同,M4 的**代码改动极小**——`SuperBlock.device_id` 字段早已存在且 `RecoverDeviceGroup`
> 已校验 `==i`,恢复链无需改签名;运营口的 N 路广播循环 M2 已写好;reactor 机制 M1–M3 已就绪。M4 的
> 本质是**把引擎从"device-0 硬编码"泛化为"device-i 参数化"的 5 处原子翻转,加上多设备测试基建**(后者
> 才是主工作量)。N=1 是翻转后的退化特例,M1–M3 测试零改动照过。
>
> **本文为详细设计**,grill 的全部裁决(A1–A5 直接采用 + B1–B5 逐条讨论)汇集于此。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P7 / M4 |
| 状态 | ⏳ **设计中(待实装)** |
| 上游依赖 | P7M1(reactor 机制)、M2(写路径)、M3(单设备多线程)全部完成;P7 阶段计划(doc/P7/README.md,P7-D7/D8);doc/P7/P7M3_single_device_mt_design.md |
| 下游依赖本里程碑 | P7M5(隔离 + 收敛);性能兑现轮(并行广播 / 流水线);P11(真盘规模与性能度量) |
| 退出判定 | 见 §8 |

---

## 1. 目标与范围

### 1.1 目标

1. **多设备端到端**:N≥2 下 Put/Get/Delete 正确,key 按 `hash%N` 分散到各设备,**同 key 恒落同一设备**。
2. **三处一致化**:路由、BlockId device 位、恢复校验三者对齐,`路由到设备 i 取到的块 dev()==i`。
3. **N 个 reactor 并行、跨设备零通信**:数据 op 路由到不同 reactor → 跨设备真并行(QPS 横向扩展的来源)。
4. **运营口 fan-out 真生效**:Snapshot/SetWalLevel/Close 触达全部 N 个 reactor、首个错误聚合。
5. **多设备恢复**:N 个设备各自独立 recover(各落各分区),含跨重启 Put/Delete/Snapshot 一致。

### 1.2 交付范围

1. **`engine/engine.cpp`**(改,核心):5 处原子翻转(§3)+ Open 的 N≤256 校验。
2. **`engine/reactor.cpp`**(改,仅加断言):`ExecuteGet` 命中后加 `dev()==device_id` 的 debug `assert`(§5);`#include <cassert>`。
3. **`test/engine/reactor_test.cpp`**(改):`MultipleDevicesFails` → `TooManyDevicesFails`(§6)。
4. **`test/engine/engine_multidevice_test.cpp`**(新建,主工作量):4 个多设备用例(§7)。
5. **`test/CMakeLists.txt`**(改):注册 `test_engine_multidevice`。
6. **`scripts/mkloop.sh`**(改):新增 `create-multi` / `cleanup-multi`(2 组 6 块)。
7. **`scripts/run-tests.sh` / `scripts/run-coverage.sh`**(改):加 `--device2` / `--wal-device2` / `--snapshot-device2`。
8. **`doc/P7/P7M4_multi_device_design.md`**(本文)。

**零改动 / 不动**:**无新增错误码**(N>256 复用 `kEngineInvalidOpts`);`engine.h` / `reactor.h` **不动**(无新成员/签名——A1 用 `super_block.device_id` 穿线、RouteKey 签名不变);恢复链(`RecoverDevice`/`ApplyWalEntry` 等)签名不变;group commit、盘上格式不碰。

### 1.3 明确不做(各有归处)

| 不做项 | 归处 |
|---|---|
| 运营口并行广播 scatter-gather(`SubmitAndWaitAll`) | 性能轮(g_wake_gen 跨 reactor 共享的基础已就位,留干净增量) |
| 多 reactor 失败的两阶段原子性 | 不做,采"遍历到底 + 取首错 + 接受部分应用"(失败=设备故障档) |
| 并行 recover(N 个设备并发恢复) | 留后(启动期优化;recover 在线程起前单线程做,P7-D11) |
| R>1(device 内再分区) | 性能轮(P7-D13;R=1 全程) |
| 真盘 QPS / 规模度量 | P11(loop 盘上 QPS 不可信) |
| 流水线 / group commit 拆壳 / TRIM / 钉核 | 性能轮 |

---

## 2. 决策汇总

| 编号 | 决策 | 结论 | grill 来源 |
|---|---|---|---|
| **A1** | 设备索引来源 | `dc.super_block.device_id`(`RecoverDeviceGroup` 已校验 ==i,可信),恢复链零签名改动 | A |
| **A2** | RouteKey 实现 | `util::RouteToDevice(key, reactors_.size())`(=`Hash(key)%N`,现成冻结) | A |
| **A3** | N 上限 | Open 校验 N≤256(DeviceId=uint8_t),超出返 `kEngineInvalidOpts` | A |
| **A4** | 多设备恢复 | **串行**(recover 在 reactor 起线程前单线程做,P7-D11);并行 recover 留后 | A |
| **A5** | QPS 扩展(exit ⑤) | 只记录、不卡门槛(loop 盘不可信,真盘度量留 P11) | A |
| **M4-B1** | 交付边界 | 5 处**原子翻转**(必须同批);N=1 是退化特例、现有测试照过;小代码 + 大测试基建 | B1 |
| **M4-B2** | 运营口广播 | **保留顺序**(广播口罕见、不碰 QPS 红线;合 P7-D2);并行 `SubmitAndWaitAll` 留性能轮;失败采"遍历到底+取首错+接受部分应用" | B2 |
| **M4-B3** | 一致性断言 | `ExecuteGet` 加 plain `assert(meta.block.dev()==device_id)`(NDEBUG 消失);exit ④ 主靠 PerDeviceRecover | B3 |
| **M4-B4** | 测试基建 | `create-multi` 建 2 组(组 1 原名 + 组 2 后缀 `2`)+ `cleanup-multi`;新 `MultiDeviceTest` + `test_engine_multidevice`;脚本加 `--device2` 等;`MultipleDevicesFails`→`TooManyDevicesFails` | B4 |
| **M4-B5** | 测试场景 | 4 用例覆盖 exit①–④ + 一个轻量多线程×多设备复合;分布性用公开 `RouteToDevice` 断言;清盘 ×2 组 | B5 |

---

## 3. 五处原子翻转(M4-B1)

### 3.1 五处改动——全用 `super_block.device_id`

| # | 处(engine.cpp) | N=1(现) | N>1(M4) |
|---|---|---|---|
| 1 | `RouteKey` :194/196 | `return 0` | `return util::RouteToDevice(key, reactors_.size())` |
| 2 | `Init` :69 | `Init(0, …)` | `Init(static_cast<DeviceId>(dc.super_block.device_id), …)` |
| 3 | `RebuildFromActive` :328 | `RebuildFromActive(0, …)` | 同上,用 `dc.super_block.device_id` |
| 4 | `ValidateRecoveredMeta` :258 | `block.dev() != 0` 拒 | `block.dev() != static_cast<DeviceId>(dc.super_block.device_id)` 拒 |
| 5 | `Open` :29 | `if (size>1) → kEngineInvalidOpts` | 删此行 + `if (size>256) → kEngineInvalidOpts`(A3) |

**为什么不需改签名**(A1):`SuperBlock.device_id`(@96,uint64_t)在 `CreateDeviceGroup(cfg, i, …)` 时写入 `i`、
在 `RecoverDeviceGroup(cfg, i, …)` 时校验三设备 `device_id==i`(不过即 `kSuperBlockDeviceIdMismatch` 拒开)。
所以进入恢复链时 `dc.super_block.device_id==i` 是**强制不变量**,`ValidateRecoveredMeta`/`RebuildAllocator`/Init
调用点都有 `dc` 在手,直接取——整条链零签名改动。

**BlockId 编码**:`raw` 高 8 位 = dev(`DeviceId`=uint8_t)、低 56 位 = block_idx;`BlockId::Make(dev, idx)`。
分配器 `Init(dev,…)` 后 `Acquire` 返回的块经 `Make` 把 dev 盖进高 8 位。**最多 256 设备**(`RouteToDevice` 自带
`assert(n_devices ∈ [1,256])`)。

**路由**:`util::RouteToDevice(key, n)` = `Hash(key) % n`,返回 `DeviceId` 可直接当 `reactors_` 下标
(N≤256,`size_t` 接收安全)。

### 3.2 为什么"必须同批"——partial 落地的具体坏状态

5 处是耦合集,只改子集留下确切坏状态:

- **只 #5(放开 N>1)不 #1**:所有 key 仍路由 reactor 0,设备 1..N-1 永远空 —— 分布全错。
- **#2(Init 设备 i)不配 #4**:块 `dev()==i`,recover 仍按 `dev()!=0` 判 → i>0 的块被**拒开**。
- **#4 不配 #2**:块仍 `dev()==0`,期望 `dev()==i` → 同样拒开。
- **#2 不配 #3**:Init 盖 i、重建却用 0 → 分配器空闲集 dev 位与活块矛盾。
- **#1 与 #2 不对齐**:路由到 i 但块 `dev()` 非 i → 违 exit ④、recover 跨不过。

即 #1–#5 要么全停 N=1 取值、要么全切 N>1 取值,中间任何子集都坏。**M4 不存在"半个多设备",是一次原子翻转。**

### 3.3 N=1 是翻转后的退化(向后兼容)

翻转后 N=1 仍正确、现有单设备测试零改动照过:`RouteToDevice(key,1)=Hash%1=0`;`device_id=0` → Init 设备 0、
校验 `dev()==0`;Open 允许 `size==1`。即"参数化"把 N=1 当 N 的特例——**泛化非替换**,不破坏 M1–M3。

---

## 4. 运营口广播(M4-B2)

### 4.1 现状:N 路循环 M2 已写,但顺序串行

`Close` / `SetWalLevel` / `Snapshot` 在 engine.cpp 已是 `for (auto& r : reactors_)` + 取首错(M2 写);但每次
`SubmitAndWait` **阻塞等完成再投下一个**,总延迟 = Σ(各 reactor)。

### 4.2 决策:M4 保留顺序,并行留性能轮

广播口(SetWalLevel/Snapshot/Close)全是**罕见管理操作、不在热路径**。M4 关心的 QPS(exit ⑤)测的是**数据 op
横向扩展**——数据 op 按 `hash%N` 路由到不同 reactor、跨设备并行,QPS 随 N 扩;运营口顺序广播**不碰 QPS、不碰
正确性**。自动快照(`MaybeRequestSnapshot`)更不是广播——在各 reactor 自己线程 op 尾段内联。

- 顺序:**零新代码**、Σ 延迟(罕见 op 可接受);
- 并行 `SubmitAndWaitAll`(先 scatter 全部、再在同一 `g_wake_gen` 上 gather 等齐、取首错):~30 行新代码 + 新 TSAN
  测试面;且 Close 是 Stop+join 不是 SubmitAndWait,**套不进** SubmitAndWaitAll(硬上会让三口处理不一致)。

合 P7-D2"首轮不纠结性能"。`g_wake_gen` 跨 reactor 共享的基础**已就位**,将来加 `SubmitAndWaitAll` 是干净局部增量、
不返工。"scatter…gather"是描述广播形态(发全部、收齐),顺序循环也是 scatter-gather、只是串行;"真生效"指
正确触达全部 N,顺序做到了——不与 README 冲突。

### 4.3 多 reactor 失败

广播中途某 reactor 失败(如 flush 错)→ **遍历到底 + 取首错 + 接受部分应用**(前面已改、失败的没改、后面继续),
不做两阶段回滚。理由:失败=设备故障档(no-fault-injection:留防御、不注入测);广播口罕见;跨 reactor 真原子性
要两阶段提交、重且越范围。这是 M2 代码已有语义,文档记"部分应用"即可。

---

## 5. 一致性断言(M4-B3)

### 5.1 这是"由构造保证"的不变量,断言作防御

链:`Put(key)` 路由 `reactors_[RouteKey(key)]`=设备 i → reactor i 从 Init(设备 i)的分配器 Acquire → 块
`dev()==i`;`Get(key)` 因 `RouteToDevice` 确定性路由到同一设备 i → 在 i 的索引 Lookup 到该块。每环由构造成立。
隐含前提:**`reactors_[i].device_id==i`**——Open 阶段一 `i` 循环建设备 i、阶段二同序 wrap 进 `reactors_[i]`。
所以 `RouteKey(key)=i → reactors_[i]`(device_id==i)。断言兜"这条对齐万一被接坏"。

### 5.2 落点

`ExecuteGet` 命中(Lookup 返 kSuccess)后:

```cpp
assert(meta.block.dev() == static_cast<DeviceId>(dc_.super_block.device_id));
```

因 `device_id == reactor 下标 == RouteKey(key)`,这就是 README 的"`meta.block.dev()==RouteKey` 一致性断言"。
plain `assert()`(同 `RouteToDevice` 自带断言风格)、**NDEBUG 下消失、release 零开销**——这是编程不变量防御、
非运行期数据状况,不返错误码、不进热路径成本。`ExecutePut`(Acquire 后)可选辅断言(分配器自检),做不做都行。
需 `#include <cassert>`。

### 5.3 强行为验证靠 recover,不读内部

测试读不到 `meta.block`,exit ④ 行为级证明靠两条:
1. **PerDeviceRecover(§7)是最强验证**:`ValidateRecoveredMeta` 恰校验 `block.dev()==device_id`(#4)。块 dev
   盖错则 recover **直接拒开**。"写两设备→Close→recover 成功"本身证明所有块 dev 正确——比读断言还硬。
2. **路由确定性**:同 key Put 后 Get 读回(两次路由同一设备才读得回)。

---

## 6. 测试基建(M4-B4)

### 6.1 mkloop:新增 `create-multi`,不动 `create`

`create` 保持原样(1 组:`cabe_test_{data,wal,snapshot}.img` → loop → `CABE_TEST_DEVICE`/`_WAL_DEVICE`/
`_SNAPSHOT_DEVICE`,数据 64MiB / WAL 16MiB / 快照 32MiB)。新增 **`create-multi`**:建**两组共 6 块**——
- 组 1:沿用现镜像名与现环境变量(向后兼容,现有 5 个 fixture 全靠它);
- 组 2:`cabe_test_data2.img` 等 → `CABE_TEST_DEVICE2` / `CABE_TEST_WAL_DEVICE2` / `CABE_TEST_SNAPSHOT_DEVICE2`。

配 `cleanup-multi`。不让 `create` 直接建 6 块(会改其既有语义、易惊到现用法);`create-multi` 是显式 opt-in、
零破坏。N=2 固定(代码支持 ≤256,测试用 2 足以证多设备)。

### 6.2 fixture + 目标

新建 `test/engine/engine_multidevice_test.cpp`:

```cpp
class MultiDeviceTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 组 1（无后缀）+ 组 2（后缀 2）
        d1_=GetEnv("CABE_TEST_DEVICE");  w1_=GetEnv("CABE_TEST_WAL_DEVICE");  s1_=GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        d2_=GetEnv("CABE_TEST_DEVICE2"); w2_=GetEnv("CABE_TEST_WAL_DEVICE2"); s2_=GetEnv("CABE_TEST_SNAPSHOT_DEVICE2");
        if (d1_.empty()||w1_.empty()||s1_.empty()||d2_.empty()||w2_.empty()||s2_.empty())
            GTEST_SKIP() << "需要两组 loop 设备（CABE_TEST_*  +  CABE_TEST_*2）";
    }
    cabe::Options MakeOpts(bool create) {
        cabe::Options o; o.create=create; o.snapshot_threshold_bytes=1024*1024;
        o.devices.push_back({d1_,w1_,s1_});
        o.devices.push_back({d2_,w2_,s2_});   // N=2
        return o;
    }
    // ...（WipeWalAndSnapshot 对两组各抹一遍，见 §7）
};
```

`test/CMakeLists.txt` 注册 `test_engine_multidevice`(仿 test_reactor:链 `cabe::engine` + `gtest_discover_tests`)。
组 2 缺变量 → `GTEST_SKIP`(最小 1 组环境也不报错)。

### 6.3 脚本

`run-tests.sh` / `run-coverage.sh` 加 `--device2` / `--wal-device2` / `--snapshot-device2`,导出对应 `CABE_TEST_*2`。
(注:脚本只 export 它认得的 `--device*`,预先 export 的 `CABE_TEST_*2` 也能透传 ctest,但加显式参数更可发现。)

### 6.4 改 `MultipleDevicesFails` → `TooManyDevicesFails`

`reactor_test.cpp` 的 `MultipleDevicesFails`(验"N=2 必被拒")编码旧不变量——放开后 2 设备合法(会去开假路径
失败、返 IO 错而非 `kEngineInvalidOpts`),必须改。改成 **`TooManyDevicesFails`**:`Open(257 个假 DeviceConfig)`
→ `kEngineInvalidOpts`(size 校验在开设备之前、不碰假路径),守 N≤256 上界。

---

## 7. 测试场景(M4-B5)

### 7.1 怎么从公开 API 观察"用了两个设备"

测试读不到 `block.dev()`,但能调公开的 `util::RouteToDevice(key, 2)` **知道**每个 key 该落哪:对自己的 key 集
算 `RouteToDevice`、断言**两设备都出现**(确认这组用例确实测 2 设备);路由一致性靠"同 key Put 后 Get 读回"。

### 7.2 四个用例(N=2,先 join 再 Close 守 M3 契约)

| 用例 | 覆盖 | 内容 + 断言 |
|---|---|---|
| **DistributedRoundTrip** | ① | 先用 `RouteToDevice` 断言 key 集跨两设备 → Put → Get 验值 → Delete → Get miss;Put/Delete 循环回收控容量 |
| **PerDeviceRecover** | ②④ | 跨两设备 Put/Delete(+ 一次 Snapshot)到**已知终态** → Close → 重开 recover(N=2)→ 验全部应存 key 读回、已删缺席。`ValidateRecoveredMeta` 校验 `dev()==device_id`,recover 成功 ⟹ **证 exit ④**。**先抹两组 WAL 环 + 快照槽头** |
| **OperationalFanOut** | ③ | Put 跨两设备 → `Snapshot()` 返 ok → 再 Put → `SetWalLevel` 轮档 → 数据仍一致 → Close 干净退出。"Close 触达全部 N":没停到的 reactor 线程会泄漏/挂 → **干净退出 + 不挂即证**(M3 活性范式) |
| **MultiThreadMultiDevice** | ⑤ + 复合 | N 调用线程并发 Put/Get/Delete 不相交 key(跨两设备)→ sync+TSAN race-free → 先 join 再 Close;记一条 QPS 日志(观察) |

### 7.3 多线程×多设备:加一个轻量复合用例

**无新 race 风险**:每 reactor share-nothing(P7-D4),多线程要么路由到同一 reactor(M3 已证的多生产者)、
要么路由到不同 reactor(互不共享)。所以复合下 race-free 是"M3 机制 × N 个独立 reactor"的推论——用例只是
TSAN 下**实测确认推论**,不重测 M3/M4。它也承载 exit ⑤ 的 QPS 观察。一个即够、不做重。

### 7.4 容量与清盘

数据盘各 64MiB(≈60 块);高量用例靠 Put→Delete 回收控在生块;PerDeviceRecover 终态在生量控小(总量远小于
2×60)。**recover 前对两组各抹一遍 WAL 环 + 快照槽头**(同 M3 清盘范式 ×2,消共用 loop 设备前序陈帧)。
io_uring 后端用 ASAN/UBSAN(io_uring 与 TSAN 不兼容),race 检查归 sync。

### 7.5 N=1 不退步

现有单设备套件(reactor/engine/recovery/concurrency)就是 N=1,翻转后照过即证向后兼容——不新增用例,全量保绿即可。

---

## 8. 退出条件

1. N=2 端到端 Put/Get/Delete 正确,key 按 hash 分散、**同 key 恒落同一设备**(DistributedRoundTrip)
2. 两设备各自 recover 后数据一致,含跨重启 Put/Delete/Snapshot(PerDeviceRecover)
3. 运营口 fan-out 正确:触达全部 N、首个错误聚合(OperationalFanOut)
4. 三处一致化无错位:路由到设备 i 取到的块 `dev()==i`(B3 断言 + PerDeviceRecover 双兜)
5. (观察)QPS 随设备数扩展——只记录,loop 盘数不可信,真盘留 P11
6. **N=1 全量回归不退步**(现有单设备套件照过);多设备 + 复合在 sync+TSAN、io_uring+ASAN 下绿

---

## 9. 关键技术备忘

1. **M4 核心代码极小**:`super_block.device_id` 早存在且 recover 已校验,5 处硬编码 0 统一换成它即可,恢复链零签名
   改动。大头在测试基建(多组设备),不在代码。
2. **三处一致化必须同批**:5 处是耦合集(§3.2),任何子集留 recover 拒开/分布错/dev 错位的坏状态。M4 是一次原子翻转。
3. **N=1 是翻转后的退化**:`Hash%1=0`、device_id=0、校验 dev==0、允许 size==1——M1–M3 测试零改动照过。泛化非替换。
4. **运营口顺序广播够用**:广播口罕见、不碰 QPS(数据 op 横向扩展才是 QPS 来源)、不碰正确性;并行 scatter-gather
   留性能轮(基础已就位)。Close 的 Stop+join 天然顺序、套不进 SubmitAndWaitAll。
5. **exit ④ 靠 recover 证**:`ValidateRecoveredMeta` 校验 `dev()==device_id`,recover 成功 ⟹ 所有块 dev 正确——
   比读内部断言还硬;debug `assert` 作为 dev/CI 期防御补刀。
6. **多线程×多设备无新 race**:M3 证了每 reactor 的 MPSC、reactor 间 share-nothing(P7-D4),复合 race-free 是推论;
   一个 TSAN 复合用例确认即可。QPS 扩展真度量留 P11(loop 盘不可信)。
