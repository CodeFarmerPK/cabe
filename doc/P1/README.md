# P1 — 单线程版核心引擎 · 设计文档索引

> P1 阶段目标：跑通完整 Put / Get / Delete 路径。**单线程、无持久化、纯 RAM 索引**。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) 第六节 "P1 — 单线程版核心引擎"。

## 状态

🚧 **未启动**（待 P0M7 收敛通过 + owner 确认启动）

## 范围摘要（出自 ROADMAP）

- `cabe::Options` / `cabe::Status` 公开类型骨架（P2 才冻结）
- `cabe::Engine` 公开类骨架：`Open` / `Put` / `Get` / `Delete` / `Close`
- **内部按 per-device 形态**：`Engine` 持有 `std::vector<DeviceContext>`，P1 内 `size() == 1`
- `struct DeviceContext { IoBackend io; FreeList free; MetaIndex index; }` 雏形
- key 路由函数 `size_t Engine::RouteKey(string_view)`，P1 内永远返回 0
- 朴素 I/O（syscall + O_DIRECT，不抽象）
- 朴素 BufferPool（对齐到 4 KiB 的 1 MiB 块池）
- 朴素 FreeList（`std::vector<BlockId>` + LIFO）
- 单层 MetaIndex（直接 `std::unordered_map<std::string, ValueMeta>`，**不引入抽象层**）
- 单 Put / Get / Delete 完整路径
- 严格 `value.size() == kValueSize` 校验
- 单元测试 + 微基准基线归档到 `bench/baselines/p1_single_thread.json`

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| 待决策梳理划分 P1M1–Mn | — | — | — |

里程碑划分待 P1 启动时由决策梳理过程确定。

## 启动条件

1. P0M7 收敛稿审阅通过（见 [doc/P0/P0M7_convergence_design.md](../P0/P0M7_convergence_design.md)）
2. owner 确认 P1 启动
3. 用 `/grill-with-docs P1` 开第一个里程碑的设计

## 已知决策点候选（为决策梳理过程提速）

> 候选清单**仅作梳理参考**，不预判答案；P1 启动时仍要重新逐条识别 + 拷问。

1. **公开 API 暴露面**：仅 `Engine`，还是 `Engine` + `Options` + `Status` 三者一并？P2 才冻结
   不等于 P1 不暴露。
2. **`Engine` 拷贝 / 移动语义**：禁止两者（管理资源类常见做法）？还是仅允许 move？
3. **错误传播风格**：所有公开方法返 `Status` 值类型 / 抛异常 / 混用？P0 已有 `cabe::err::*` 段位
   错误码，但未定 `Status` 类型本身。
4. **`DeviceContext` 单 / 多例**：`Engine` 持有 `std::vector<DeviceContext>` 已锁定；但 P1 内
   `size() == 1` 的"单例"是否仍走 vector 容器（保持与多 device 形态一致）？
5. **朴素 BufferPool 对齐策略**：编译期常量 4 KiB 对齐，还是运行时探测 `getpagesize()`？
6. **O_DIRECT 与 BufferPool 协作边界**：所有 I/O 都过 BufferPool（统一缓冲），还是部分小读
   绕过？这关系到 `pread / pwrite` 的对齐承诺。
7. **MetaIndex 的 key 拥有权**：`std::unordered_map<std::string, ValueMeta>` 还是
   `std::string_view` 作 key（要求调用方持有 key buffer）？影响 P3 抽象层接口设计。
8. **`RouteKey` P1 返回 0 的占位实现**：直接 `return 0`，还是调 `cabe::util::Hash(key) % 1`
   保持代码路径一致？这关系到 P3 多 device 时改动量。

## 命名与目录约定

参见 [doc/P0/README.md](../P0/README.md) — 沿用 `P<阶段>M<里程碑>_<主题>_design.md`。
