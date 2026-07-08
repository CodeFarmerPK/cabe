# Cabe P8-M5 设计：bench、归档与收敛

> 本里程碑是 P8 的最后一刀：补齐 P8 专属 bench、归档 P8 bench 数据、完成全量回归与覆盖率验证，
> 并把 P8 阶段从“零拷贝路径已实现”收束为“阶段完成”。P8M5 不再扩展新的零拷贝能力，不新增公开
> 写入接口，不引入 SPDK 依赖，也不在 loop 设备上给出性能收益结论。
>
> **本文为 P8M5 详细设计 + P8 收敛稿合一**。前半部分说明 M5 的 bench、归档、验证和文档收敛设计；
> 后半部分回链 P8-D1 ~ P8-D11 与 P8M1 ~ P8M4，核销 P8 总退出条件，并明确后续债务流向。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P8 / M5 |
| 状态 | ⏳ 设计完成，待实现 |
| 上游依赖 | P8M1 公开 `ValueBuffer` API 已实装；P8M2 `ValueBufferPool` 已实装；P8M3 `Put` 路径选择已实装；P8M4 后端写入协议与 `io_uring` 注册缓冲区已实装 |
| 下游依赖本里程碑 | P9 B+树元数据索引；P10 SPDK 后端；性能兑现阶段；P11 真盘规模验证 |
| 退出判定 | 见 §13 |

---

## 1. 目标与范围

### 1.1 目标

1. **补齐 P8 专属 bench**：新增能区分三类 value 来源的 `Put` 微基准，覆盖非对齐自备值内存、对齐自备值内存和 Cabe `ValueBuffer`。
2. **归档 P8 bench 数据**：沿用 `bench/baselines/<阶段>/` + google-benchmark 原生 JSON 的既有方式，新增 `bench/baselines/p8/`。
3. **完成正式验证矩阵**：跑 sync 四档、`io_uring` 三档、sync strict 覆盖率和 `io_uring` Release bench。
4. **完成 P8 收敛文档**：回链 P8 总体决策、P8M1 ~ P8M4 交付、P8 总退出条件和后续债务。
5. **同步项目状态**：在所有退出条件核销后，更新 P8 README、根 README、ROADMAP 和 bench 归档索引。

### 1.2 交付范围

| 交付物 | 类别 |
|---|---|
| `bench/engine/engine_value_put_bench.cpp` | bench 代码 |
| `bench/CMakeLists.txt` 增加 `bench_engine_value_put` 目标 | bench 构建 |
| `bench/baselines/p8/*.json` | bench 数据手动归档 |
| `bench/baselines/README.md` 补齐 P7 / P8 归档说明 | 文档 |
| `doc/P8/P8M5_bench_convergence_design.md` | 本文档 |
| `doc/P8/README.md` 状态、里程碑、退出条件同步 | 文档 |
| 根 `README.md` 与 `ROADMAP.md` P8 状态同步 | 文档 |

### 1.3 明确不做

| 不做项 | 归属 |
|---|---|
| 新增公开写入接口，例如 `Put(ValueBuffer&&)` | 不做 |
| 新增公开路径统计接口 | 不做 |
| 新增高频路径日志 | 不做 |
| SPDK 后端实现 | P10 |
| SPDK 大页内存池真实接入 | P10 |
| 写入流水线、多写入在飞、提交顺序水位 | 性能兑现阶段 |
| NUMA、绑核、缓存行布局、池容量调优 | 性能兑现阶段 |
| 真盘性能结论和规模化设备验证 | P11 |
| 完整生产级用户手册和运维文档 | P11 或发布阶段 |
| 借 bench 数字做产品代码性能优化 | 不做 |

### 1.4 产品代码冻结原则

P8M5 原则上冻结产品代码。允许的产品代码改动只有一种：全量测试、覆盖率或 bench 暴露了 P8M1 ~ P8M4
已经承诺的正确性缺陷，例如主零拷贝路径写入错误、键绑定回退错误、跨设备回退错误、严格 `Close`
边界失效或注册缓冲区资源泄漏。

不允许因为 loop 设备上的 bench 数字不理想而调整产品代码。P8M5 的 bench 数据只作为路径观察和历史样本。

---

## 2. 决策汇总

| 编号 | 决策 | 结论 |
|---|---|---|
| **P8M5-D1** | M5 定位 | P8M5 是 bench、归档、验证、文档与状态收敛里程碑，不继续扩展新的零拷贝能力。 |
| **P8M5-D2** | bench 覆盖对象 | 保留现有 `bench_engine` 历史口径，新增 P8 专属 `bench_engine_value_put`，覆盖三类 value 来源。 |
| **P8M5-D3** | 三类 value 来源 | 非对齐自备值内存走复制回退路径；对齐自备值内存走条件式零拷贝路径；Cabe `ValueBuffer` 走主零拷贝路径。 |
| **P8M5-D4** | 数据准备和计时 | value 填充、缓冲区分配和预热不计入主写入耗时；计时区间只覆盖 `Engine::Put` 主体。 |
| **P8M5-D5** | WAL 级别覆盖 | P8 专属 `Put` bench 继续覆盖 WAL 级别 1 / 2 / 3 / 4，与现有 `bench_engine` 对齐。 |
| **P8M5-D6** | 性能对比口径 | bench 只记录测试数据并保存文件，不做性能优劣结论；当前 loop 设备做性能对比意义有限。 |
| **P8M5-D7** | 路径可观测性 | 不新增公开路径统计接口、不新增高频日志；通过输入形态、内部测试和 bench 组合观察路径。 |
| **P8M5-D8** | P8M5 实现边界 | M5 正常只改 bench、手动归档数据和文档；不修改 bench 脚本，不新增 SPDK、写入流水线或公开 API。 |
| **P8M5-D9** | 归档文件 | 在 `bench/baselines/p8/` 归档 google-benchmark 原生 JSON，新增 `engine_value_put.io_uring.gcc.json`。 |
| **P8M5-D10** | 文档同步 | P8M5 负责收敛稿、P8 总索引、路线图、根 README 与 bench 归档索引同步；不新增完整用户手册。 |
| **P8M5-D11** | 退出条件 | 以正确性、覆盖率、bench 归档、文档同步和债务收口作为 P8M5 与 P8 整体收官标准，不设 loop 性能硬门槛。 |
| **P8M5-D12** | 后续债务 | 建立债务流向表，按 P9、P10、性能兑现阶段和 P11 归类；M5 不新增功能。 |
| **P8M5-D13** | 实施顺序 | 按“bench 实现 -> 冒烟 -> 全量验证 -> 正式归档 -> 收敛文档 -> 状态同步”实施。 |
| **P8M5-D14** | 文档结构 | `P8M5_bench_convergence_design.md` 同时作为 P8M5 详细设计和 P8 阶段收敛稿，不拆额外收敛文档。 |
| **P8M5-D15** | bench 脚本 | 不修改 `run-bench.sh`；脚本只负责构建、运行 bench 并生成 JSON，P8 数据由开发者手动归档。 |
| **P8M5-D16** | 验证矩阵 | 正式矩阵为 sync 四档、`io_uring` 三档、sync strict 覆盖率和 `io_uring` Release bench 归档。 |
| **P8M5-D17** | 产品代码冻结 | 原则上冻结产品代码；只允许修复 P8 已承诺语义的正确性缺陷；不做性能优化。 |
| **P8M5-D18** | 数据解释 | 只解释 bench 数据的路径含义，不在 loop 设备上给出零拷贝性能提升结论。 |
| **P8M5-D19** | 复现信息 | 不新增独立环境报告；在本文和 `bench/baselines/README.md` 记录命令、设备要求、归档方式和限制。 |
| **P8M5-D20** | 交付物和下一阶段入口 | 在收敛稿中列出交付物清单与 P9/P10/性能兑现/P11 入口，不提前展开后续阶段详细设计。 |

---

## 3. P8 专属 bench 设计

### 3.1 新增目标

新增目标：

```text
bench_engine_value_put
```

建议文件：

```text
bench/engine/engine_value_put_bench.cpp
```

该目标只测 P8 写入路径，不替代现有 `bench_engine`。现有 `bench_engine` 继续保留历史对比口径：

```text
bench_engine:
  BM_Put / BM_Get / BM_Delete × WAL 1/2/3/4

bench_engine_value_put:
  BM_PutUnalignedExternal / BM_PutAlignedExternal / BM_PutValueBuffer × WAL 1/2/3/4
```

### 3.2 三类 `Put` 基准

| benchmark | value 来源 | 构造方式 | 预期路径 |
|---|---|---|---|
| `BM_PutUnalignedExternal` | 应用端普通内存 | 分配 `kValueSize + 1`，传入偏移 1 字节后的视图 | 复制回退路径 |
| `BM_PutAlignedExternal` | 应用端对齐内存 | 使用 1 MiB 对齐分配，传入完整 1 MiB 视图 | 条件式零拷贝路径 |
| `BM_PutValueBuffer` | Cabe `ValueBuffer` | `Engine::AllocateValueBuffer(key)` 后填充，再 `Put(key, buffer.view())` | 主零拷贝路径 |

关键点：

1. 三类 bench 的 value 长度都必须是固定 1 MiB。
2. 非对齐自备值内存必须稳定制造地址不对齐，而不是依赖普通分配器的偶然结果。
3. 对齐自备值内存用于观察当前 `io_uring` 过渡后端下的条件式直接写入路径，但不代表未来 SPDK 下普通对齐内存一定可直接写入。
4. Cabe `ValueBuffer` 使用分配 key 与 `Put` key 完全一致的路径，确保命中主零拷贝路径。

### 3.3 WAL 级别参数化

沿用现有 `bench_engine` 的参数化方式：

```text
->Arg(1)->Arg(2)->Arg(3)->Arg(4)
```

每个 benchmark 覆盖四个 WAL 级别：

| WAL 级别 | 意义 |
|---|---|
| 1 | value FUA + WAL 同步 |
| 2 | value FUA + WAL 攒批 |
| 3 | value 非 FUA + WAL 同步 |
| 4 | value 非 FUA + WAL 攒批 |

P8M5 不根据级别裁剪矩阵。四级都保留，便于与 P6/P7 的 bench 输出结构保持一致。

### 3.4 数据填充和计时边界

计时区间只覆盖 `Engine::Put`。以下操作不计入主写入耗时：

1. 自备值内存的分配；
2. Cabe `ValueBuffer` 的分配；
3. value 内容填充；
4. 必要的预热；
5. 删除 / 重开 / 重填等测试维护动作。

推荐写法：

```cpp
state.PauseTiming();
// 分配或准备下一次 Put 使用的 value。
state.ResumeTiming();
auto s = engine.Put(key, DataView{value});
```

对 `BM_PutValueBuffer`，每次迭代是否重新分配 `ValueBuffer` 需要谨慎：如果把分配成本计入，就不再是纯写入路径 bench；
如果完全复用同一个 key，会变成覆盖写同一条记录，旧块回收行为与普通新增写不同。P8M5 建议使用固定数量的预分配
缓冲区轮转，每轮在暂停计时区间填充内容，计时区间只覆盖对应 key 的 `Put`。

### 3.5 设备与容量

P8 专属 bench 继续使用 bench 大设备，而不是测试小设备。设备准备沿用现有脚本能力：

```bash
./scripts/mkloop.sh create-bench
```

多线程 bench 仍需要两组 bench 设备：

```bash
./scripts/mkloop.sh create-bench-multi
```

P8M5 不改变设备尺寸策略。若某个 benchmark 因写满而跳过或失败，应优先缩短 `--benchmark_min_time` 或确认 bench 设备是否正确创建，
不把写满重开计入性能结论。

---

## 4. bench 运行与归档口径

### 4.1 运行口径

P8 正式归档使用：

| 项 | 口径 |
|---|---|
| 后端 | `io_uring` |
| 编译器 | `g++` |
| 构建类型 | Release |
| 格式 | google-benchmark 原生 JSON |
| 重复次数 | 沿用 `run-bench.sh` 既有 `--benchmark_repetitions=5` |
| 聚合 | 沿用 `--benchmark_report_aggregates_only=true` |

sync 后端继续用于正确性和线程检测器验证，不出 P8 性能归档数据。

### 4.2 归档目录与文件

新增目录：

```text
bench/baselines/p8/
```

P8M5 归档文件：

```text
bench/baselines/p8/engine.io_uring.gcc.json
bench/baselines/p8/engine_mt.io_uring.gcc.json
bench/baselines/p8/wal_concurrency.io_uring.gcc.json
bench/baselines/p8/engine_value_put.io_uring.gcc.json
```

前三个文件延续 P7 归档口径，第四个文件是 P8 新增的零拷贝写入路径基准。

命名规则沿用：

```text
<bench>.<backend>.<compiler>.json
```

其中 `<bench>` 去掉可执行文件名前缀 `bench_`：

| 可执行文件 | 归档文件前缀 |
|---|---|
| `bench_engine` | `engine` |
| `bench_engine_mt` | `engine_mt` |
| `bench_wal_concurrency` | `wal_concurrency` |
| `bench_engine_value_put` | `engine_value_put` |

### 4.3 手动归档流程

`scripts/run-bench.sh` 不绑定任何具体里程碑，也不增加 P8 专属归档参数。脚本只负责：

1. 构建 bench 目标；
2. 自动发现并运行 `bench_*` 可执行文件；
3. 在 `build-bench/<compiler>-<backend>-release/` 下生成 google-benchmark JSON；
4. 用 `--show=<file>` 解析已有 JSON。

P8M5 的 bench 数据由开发者手动归档。流程如下：

```bash
./scripts/run-bench.sh --backend=io_uring --compiler=g++ --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
mkdir -p bench/baselines/p8
cp build-bench/gcc-io_uring-release/bench_engine.json bench/baselines/p8/engine.io_uring.gcc.json
cp build-bench/gcc-io_uring-release/bench_engine_mt.json bench/baselines/p8/engine_mt.io_uring.gcc.json
cp build-bench/gcc-io_uring-release/bench_wal_concurrency.json bench/baselines/p8/wal_concurrency.io_uring.gcc.json
cp build-bench/gcc-io_uring-release/bench_engine_value_put.json bench/baselines/p8/engine_value_put.io_uring.gcc.json
```

手动归档时必须先确认目标文件是否已经存在，避免覆盖历史归档。P8M5 不通过脚本自动覆盖或自动比较归档数据。

### 4.4 数据解释口径

P8M5 的 bench 数据只表达三件事：

1. 三类 value 来源都能稳定跑通；
2. 数据已经按统一口径归档；
3. 后续阶段可以回看这批历史样本。

P8M5 不写如下结论：

```text
ValueBuffer 比普通写快 X%
对齐自备内存比非对齐自备内存快 Y%
P8 已经证明真实设备上的零拷贝收益
```

原因是当前 loop 设备存在共享背存、宿主机缓存、后台写回和调度噪声。P8 的结构价值来自写入路径已经打通，
不是来自这批 loop 数字本身。

---

## 5. 正确性测试与覆盖率验证

### 5.1 正式验证矩阵

P8M5 沿用 P7 的 7 配置矩阵：

| 后端 | 配置 | 目的 |
|---|---|---|
| sync | asan | 内存错误检查 |
| sync | tsan | 线程竞争检查 |
| sync | ubsan | 未定义行为检查 |
| sync | release | release 正确性 |
| `io_uring` | asan | `io_uring` 路径内存错误检查 |
| `io_uring` | ubsan | `io_uring` 路径未定义行为检查 |
| `io_uring` | release | `io_uring` release 正确性 |

`io_uring` 不跑 TSAN，沿用 P4/P7 既定边界。P8 的跨线程并发面主要由 reactor模型与 `ValueBufferPool` 无锁结构承担，
sync + TSAN 继续作为线程检测器证据。

### 5.2 覆盖率

覆盖率继续使用 sync 后端：

```bash
./scripts/run-coverage.sh --backend=sync --strict --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
```

如果 gcovr 输出与编译器插桩有关的 info / warning，但最终 strict 门槛通过，则记录说明即可。P8M5 不为消除非业务告警改产品代码。

### 5.3 P8 专属路径测试要求

P8M5 收官时至少确认以下路径已有测试覆盖：

| 场景 | 期望 |
|---|---|
| 普通自备值内存 | 可写入成功，必要时复制回退 |
| 非对齐自备值内存 | 复制回退，写入结果正确 |
| 对齐自备值内存 | 满足当前后端能力时直接写入，结果正确 |
| Cabe `ValueBuffer`，键匹配且设备匹配 | 命中主零拷贝路径，结果正确 |
| Cabe `ValueBuffer`，键不匹配 | 复制回退，写入成功 |
| Cabe `ValueBuffer`，跨设备 | 复制回退，写入成功 |
| 池耗尽 | 分配返回明确错误，不阻塞等待 |
| `Close` 严格边界 | `Close` 等待已分配缓冲区释放；完成后拒绝旧打开周期资源请求 |
| `io_uring` 注册缓冲区 | 注册失败则 `Open` 失败并回滚；注册成功时主路径可写入 |
| 复制回退与零拷贝路径一致性 | `Get` 读回内容一致，CRC / WAL / 索引语义不退化 |

### 5.4 命令模板

最终收官命令在实现完成后按实际设备回填。模板如下：

```bash
./scripts/run-tests.sh --backend=sync --asan --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-tests.sh --backend=sync --tsan --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-tests.sh --backend=sync --ubsan --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-tests.sh --backend=sync --release --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-tests.sh --backend=io_uring --asan --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-tests.sh --backend=io_uring --ubsan --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-tests.sh --backend=io_uring --release --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-coverage.sh --backend=sync --strict --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
./scripts/run-bench.sh --backend=io_uring --compiler=g++ --device=/dev/loop0 --wal-device=/dev/loop1 --snapshot-device=/dev/loop2 --device2=/dev/loop3 --wal-device2=/dev/loop4 --snapshot-device2=/dev/loop5
```

---

## 6. 文档收敛与状态同步

### 6.1 需要更新的文档

| 文件 | 改动 |
|---|---|
| `doc/P8/P8M5_bench_convergence_design.md` | 本文，P8M5 详细设计 + P8 收敛稿 |
| `doc/P8/README.md` | P8 状态、M5 状态、P8M5 归档口径、总退出条件核销 |
| `bench/baselines/README.md` | 补齐 P7 已存在归档；新增 P8 归档说明、复现命令和解释限制 |
| 根 `README.md` | P8 状态标记完成 |
| `ROADMAP.md` | P8 段状态标记完成，保留 SPDK 后续主线说明 |

状态同步必须放在最后执行。只有全量验证、覆盖率、bench 归档和收敛稿都完成后，才能把 P8 标记为完成。

### 6.2 不新增完整用户手册

P8M5 不新增独立用户手册，但必须在 P8 README 或本文中补齐以下公开约束：

1. `ValueBuffer` 固定 1 MiB；
2. `ValueBuffer` 与分配时的键绑定；
3. `ValueBuffer` 生命周期必须覆盖 `Put` 调用；
4. `Close` 是严格打开周期边界；
5. 池耗尽返回错误，不阻塞等待；
6. 复制回退不是错误；
7. 应用端自备值内存只是条件式零拷贝；
8. 当前 `io_uring` 是过渡实现，未来 SPDK 是主要方向。

### 6.3 复现信息

P8M5 不新增独立环境报告文件。复现信息写入本文和 `bench/baselines/README.md`：

| 信息 | 内容 |
|---|---|
| 后端 | `io_uring` |
| 编译器 | `g++` |
| 构建类型 | Release |
| 设备 | bench 大设备；多线程 bench 需要两组设备 |
| 运行脚本 | `scripts/run-bench.sh` |
| 归档方式 | 开发者手动复制 JSON 到 `bench/baselines/p8/` |
| 查看方式 | `scripts/run-bench.sh --show=<json>` |
| 限制 | loop 设备数据只作为历史样本和路径观察 |

---

## 7. P8 决策回链

| 编号 | 决策 | P8M5 收敛确认 |
|---|---|---|
| P8-D1 | 术语边界 | 文档统一使用“值内存 / Cabe 值缓冲区 / 值缓冲区池 / 零拷贝写入路径 / 复制回退路径 / 键绑定”。 |
| P8-D2 | 公开写入接口 | `Put` 保持唯一公开写入接口；P8M5 不新增强制零拷贝接口。 |
| P8-D3 | 应用端自备值内存 | P8 专属 bench 覆盖非对齐与对齐自备值内存，分别观察复制回退和条件式零拷贝。 |
| P8-D4 | Cabe 值缓冲区 | P8 专属 bench 覆盖 Cabe `ValueBuffer` 主路径。 |
| P8-D5 | 值缓冲区池抽象 | P8M5 不改池抽象，只验证容量、耗尽、释放和关闭边界。 |
| P8-D6 | 键绑定与设备归属 | P8M5 收官测试确认键不匹配和跨设备场景复制回退。 |
| P8-D7 | 后端写入协议 | P8M5 通过 `io_uring` 路径和 bench 归档验证 `IoWriteBuffer` 协议可工作。 |
| P8-D8 | SPDK 预留边界 | P8M5 文档明确 P8 不引入 SPDK，P10 承接真实 SPDK 后端。 |
| P8-D9 | 资源配置与耗尽语义 | P8M5 验证池耗尽返回明确错误，不阻塞等待。 |
| P8-D10 | 测试可观测性 | 不新增公开统计；用测试和 bench 证明路径。 |
| P8-D11 | bench 测试口径 | P8M5 实现三类 `Put` bench，并归档 JSON。 |

---

## 8. P8 五个里程碑摘要

| 里程碑 | 交付 | 状态 |
|---|---|---|
| P8M1 公开接口与术语落地 | 新增 `ValueBuffer`、`ValueBufferResult`、`Engine::AllocateValueBuffer` 和配置项 | ✅ 已实装 |
| P8M2 值缓冲区池抽象 | 每设备无锁 `ValueBufferPool`、来源识别、设备归属、严格关闭边界 | ✅ 已实装 |
| P8M3 `Put` 路径选择集成 | `Engine::Put` 预识别来源，`Reactor::ExecutePut` 选择直接写入或复制回退 | ✅ 已实装 |
| P8M4 后端写入协议与 `io_uring` 注册缓冲区 | `IoWriteBuffer`、sync 兼容、`io_uring` fixed buffer 写入、注册失败回滚 | ✅ 已实装 |
| P8M5 bench、归档与收敛 | P8 专属 bench、P8 数据归档、全量验证、文档和状态收敛 | ⏳ 待实现 |

---

## 9. P8 总退出条件核销

P8 README 的总退出条件在 M5 收官时逐条核销：

| 退出条件 | 核销方式 |
|---|---|
| `Engine::Put` 公开语义保持不变 | P8M1 ~ M4 已保持统一 `Put`；M5 不新增写入接口 |
| 应用端可以继续传入普通 `DataView` | 回归测试和 `BM_PutUnalignedExternal` 覆盖 |
| Cabe 提供可公开使用的 `ValueBuffer` 分配接口 | P8M1 / M2 已实装；M5 文档补约束 |
| Cabe `ValueBuffer` 可以进入主零拷贝写入路径 | P8M3 / M4 已实装；M5 bench 和测试确认 |
| 应用端自备值内存可以条件式进入零拷贝路径 | P8M3 已实装；M5 对齐自备内存 bench 覆盖 |
| 不满足零拷贝条件的 value 自动复制回退 | P8M3 已实装；M5 非对齐自备内存、键不匹配、跨设备测试确认 |
| 复制回退与零拷贝路径写入结果一致 | Engine 行为测试确认 `Put/Get` 内容一致 |
| WAL、CRC、索引更新和 block 回收语义不退化 | 全量测试矩阵确认 |
| `io_uring` 后端支持注册缓冲区写入 | P8M4 已实装；`io_uring` 三档测试和 M5 bench 确认 |
| 同步后端仍可用于测试 | sync 四档测试和覆盖率确认 |
| P8 bench 能与 P7 基线比较 | `bench/baselines/p8/` 与 P7 同格式归档 |
| SPDK 未来接入边界清晰，不需要推翻 P8 公开 API | P8M4 / M5 文档明确 P10 接入点 |

---

## 10. 后续债务流向

| 债务 | 流向 |
|---|---|
| B+树元数据索引 | P9 |
| SPDK 后端真实实现 | P10 |
| SPDK 大页内存池与 Cabe `ValueBufferPool` 对接 | P10 |
| SPDK 下应用端自备内存的直接写入能力判定 | P10 |
| 跨进程外部内存导入 | 不进入 P8，后续单独评估 |
| 写入流水线、多写入在飞、提交顺序水位 | 性能兑现阶段 |
| reactor模型下更深的后端异步化 | 性能兑现阶段 / P10 |
| NUMA、绑核、缓存行布局、池容量调优 | 性能兑现阶段 |
| 真实裸盘 / NVMe 性能结论 | P11 |
| 多设备规模化验证、运维文档、生产环境观察 | P11 |
| 完整生产级用户手册 | P11 或发布阶段 |

P8M5 的收官标准是零拷贝写入路径主路径化完成，不是最终高性能存储栈完成。

---

## 11. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| bench 误把填充成本算入写入成本 | P8 路径数据失真 | 分配、填充、预热放在暂停计时区间 |
| `ValueBuffer` bench 计入分配成本 | 不能表达主写入路径成本 | 使用预分配或暂停计时分配，计时只覆盖 `Put` |
| loop 设备数据被误读为真实性能结论 | 文档误导 | 只归档数据，不写提升百分比 |
| 新增 bench 影响旧 bench 口径 | P6/P7 历史对比断裂 | 新增独立目标，不改 `bench_engine` 语义 |
| 手动归档覆盖旧文件 | 丢失历史数据 | 归档前检查目标文件是否存在；不使用自动覆盖 |
| 文档提前标完成 | 状态与事实不一致 | 状态同步放在最后 |
| P8M5 膨胀成性能优化阶段 | 延误收官 | 产品代码冻结，只修正确性缺陷 |
| SPDK 边界表述过满 | 误以为 P8 已完成 SPDK | 文档明确 SPDK 真实接入在 P10 |

---

## 12. 实施顺序

P8M5 按以下顺序实施：

1. 新增 `bench_engine_value_put`。
2. 更新 `bench/CMakeLists.txt`。
3. 跑新增 bench 冒烟，确认能产出 JSON。
4. 跑全量正确性测试与覆盖率。
5. 跑 `io_uring` + `g++` + Release 正式 bench。
6. 手动归档 JSON 到 `bench/baselines/p8/`。
7. 更新 `bench/baselines/README.md`。
8. 根据实测结果回填本文的执行状态。
9. 更新 `doc/P8/README.md`、根 `README.md` 和 `ROADMAP.md`。

状态同步必须是最后一步。

---

## 13. 退出条件与收官判定

### 13.1 P8M5 退出条件

1. `bench_engine_value_put` 编译通过并能产出 JSON。
2. sync 四档全量测试通过。
3. `io_uring` 三档全量测试通过。
4. sync strict 覆盖率通过。
5. `io_uring` + `g++` + Release bench 完成并生成 JSON。
6. P8 JSON 手动归档到 `bench/baselines/p8/`，且不覆盖已有文件。
7. `bench/baselines/README.md` 补齐 P7 与 P8 归档说明。
8. 本文作为 P8M5 详细设计和 P8 收敛稿审阅通过。
9. P8 README、根 README、ROADMAP 状态同步完成。
10. P8 后续债务流向表写清楚。

### 13.2 P8 收官判定

满足 §13.1 后，P8 可以标记为完成：

```text
P8 = M1 公开接口
   + M2 值缓冲区池
   + M3 Put 路径选择
   + M4 后端写入协议与 io_uring 注册缓冲区
   + M5 bench、归档、验证和收敛
```

P8 完成不等于 SPDK 完成，也不等于最终生产性能调优完成。P8 完成的准确含义是：

```text
零拷贝写入路径主路径化已经完成；
Cabe 自管 ValueBuffer 主路径已经打通；
应用端自备内存条件式路径和复制回退路径已经打通；
io_uring 过渡后端完成注册缓冲区验证；
未来 SPDK 接入不需要推翻公开 Put API。
```

---

## 14. 交付物清单与下一阶段入口

### 14.1 P8M5 完成后必须存在

1. `bench_engine_value_put` 基准测试目标。
2. `bench/baselines/p8/` 下的 P8 归档 JSON。
3. `bench/baselines/README.md` 中的 P7 / P8 归档说明。
4. `doc/P8/P8M5_bench_convergence_design.md`。
5. `doc/P8/README.md` 中 P8 完成状态和 M1 ~ M5 核销。
6. 根 `README.md` 与 `ROADMAP.md` 中 P8 完成状态。
7. P8 后续债务流向表。
8. 全量测试、覆盖率、bench 归档流程的可复现命令。

### 14.2 下一阶段入口

| 阶段 | 入口 |
|---|---|
| P9 | B+树元数据索引，承接当前元数据索引抽象层 |
| P10 | SPDK 后端，承接 `ValueBufferPool`、`IoWriteBuffer` 和槽位身份抽象 |
| 性能兑现阶段 | 写入流水线、深度异步化、NUMA、绑核和池容量调优 |
| P11 | 真盘规模验证、多设备规模化、运维文档和生产环境观察 |

P8M5 不提前展开 P9 / P10 的详细设计。
