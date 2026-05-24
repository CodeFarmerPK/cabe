# P2 — 公开 API 契约冻结 + Forward-compat 论证 · 设计文档索引

> P2 阶段目标：把公开 API 冻结到 v1.0，通过 PoC 验证 API 形态能撑到 P11。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) 第七节 "P2 — 公开 API 契约冻结 + Forward-compat 论证"。

## 状态

🚧 **未启动**（待 P1 阶段所有里程碑完成 + owner 确认）

## 范围摘要（出自 ROADMAP）

- **不实现并发安全**；明确"P2–P6 单线程访问，多线程语义在 P7"
- **不引入 mutex / shared_mutex**
- `Options` 形态最终化（含 `std::vector<DeviceContext> devices`、`reactors_per_device` 不暴露
  按 D9 v1.0 硬编码 R=1、其他字段全部预留 reserved）
- `Status` 错误码空间评估，确认可容纳 SPDK / WAL recovery / 多 device 故障
- `Engine::Open` 幂等语义，Open / Close 状态机
- Put API：`Status Put(string_view key, DataView value)` + 运行期 `value.size() != kValueSize` 拒绝
- 零拷贝 API 在此定型（实现层 P8 落地，API 不变）
- **声明**：P3 将引入 `IoBackend` 与 `MetaIndex` 抽象层
- **Forward-compat PoC**（归档到 `doc/P2/`，具体稿名 P2 启动时按 P0 模式划分）：
  1. sync API + lock-free reactor：4 线程并发到目标 QPS
  2. 多 device hash 路由：2 个伪 device，验证 per-device MetaIndex 互不共享
  3. 零拷贝 buffer ownership：`BufferHandle` 在 mmap / `io_uring` registered / SPDK pool
     三种来源下接口一致
  4. recovery 隔离：一个 device 索引重建失败，其他不受影响

**ROADMAP 退出条件**：四 PoC 通过、API 符号清单审阅通过，**直到 P11 完成不允许破坏**。

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| 待决策梳理划分 P2M1–Mn | — | — | — |

里程碑划分待 P2 启动时由决策梳理过程确定。

## 启动条件

1. P1 阶段所有里程碑完成 + 单元测试 / 微基准基线归档到 `bench/baselines/p1_single_thread.json`
2. owner 确认 P2 启动
3. 用 `/grill-with-docs P2` 开第一个里程碑的设计

## 已知决策点候选（为决策梳理过程提速）

> 候选清单**仅作梳理参考**，不预判答案；P2 启动时仍要重新逐条识别 + 拷问。

1. **API 版本号方案**：语义化（major.minor.patch）还是单调整数？版本号是否进 `Options` /
   `Status` 字段还是仅文档承诺？
2. **Forward-compat PoC 形态**：4 个 PoC 各自独立可执行，还是合并到一个 PoC 进程内多场景跑？
   PoC 代码归档在 `bench/forward_compat/` 还是单独 `forward_compat/` 顶层目录？
3. **错误码 ABI 冻结范围**：仅冻结已分配的码值 + 段位划分，还是连段内编号策略一起冻结？
4. **公开类型的 ABI 兼容承诺**：结构体布局 / 枚举值 / 函数签名分别承诺到什么程度？
   `Options` 的 reserved 字段如何在不破坏 ABI 的前提下未来扩展？
5. **`Engine` 析构语义**：未调 `Close` 直接析构是 UB / 自动 Close / 抛异常？P1 决策反向波及到 P2 冻结。
6. **`Put` 失败的部分写语义**：value 已开始写、刚好写一半 power loss 时的承诺（关系到 P5 WAL 设计）。
   P2 仅冻结 API，但 API 的"承诺语义"已经隐含约束 P5 实现。
7. **零拷贝 `BufferHandle` 接口形态**：值类型 vs 引用计数智能指针？所有权转移语义？
8. **`Status` 是 `int` 错误码 + 文本，还是带类型层级的 struct**？P0 错误码仅是 `int`，
   `Status` 类型在 P2 才出现。

## 命名与目录约定

参见 [doc/P0/README.md](../P0/README.md) — 沿用 `P<阶段>M<里程碑>_<主题>_design.md`。
