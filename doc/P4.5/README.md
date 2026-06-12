# P4.5 — 块分配器改造 · 设计文档索引

> P4.5 阶段目标：将朴素 FreeList（LIFO 栈）替换为可插拔的块分配器抽象层（C++20 concept），
> 默认实现为固定大小环形队列（FIFO）。为 P5 崩溃恢复提供重建接口，为 P7 无锁多线程
> 和多设备扩展打下基础。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P4.5 — FreeList 改造"。

## 状态

✅ **已完成**（P4.5M3 收敛通过）

## 设计背景与决策依据

### 为什么不采用三容器轮换方案

ROADMAP 原文规划了"三容器（active / sorting / recycled）轮换 + shard 内升序分配 + 异步排序"
方案。经过深入分析，该方案不再适用于当前设计：

1. **升序分配对 1M 块无性能价值**：cabe 的块大小为 1M，在 NVMe SSD 上"升序但不连续的分配"
   与"完全随机分配"的性能差异趋近于零（< 1%）——因为 FTL 映射后逻辑升序不等于物理顺序。
2. **升序无法长期维持**：随着 cabe 持续运行（不断增删），空闲块号的回收顺序取决于用户的
   删除模式——本质上不可控。运行一段时间后，空闲块号自然趋向无序，排序无法改变这个事实。
3. **三容器的核心优势在无锁分离**：但 P4.5 阶段仍为单线程，无锁优势体现不出来；到 P7
   多线程时，环形队列同样支持无锁扩展（`head` / `tail` 改为原子变量），且更简单。
4. **复杂度不匹配**：三容器轮换 + 后台排序线程的实现和测试成本高，用于解决一个
   "对 1M 块无价值"的问题，投入产出比不合理。

### 为什么选择环形队列（FIFO）

1. **FIFO 语义优于 LIFO**：LIFO（当前方案）导致热块反复使用——刚释放的块立刻被重新分配；
   FIFO 让块使用更均匀——刚释放的块排到队尾，等其他块用过一轮才轮到，对 SSD 磨损均衡更友好。
2. **固定大小**：设备块数在 Open 时已知且不变，环形队列一次分配、零碎片。
3. **O(1) 操作**：获取和回收都是常数时间。
4. **无锁扩展自然**：P7 阶段 `head` / `tail` 改为原子变量即可实现单生产者单消费者无锁队列。

### 为什么做抽象层

不同应用场景的 I/O 模式不同。cabe 当前只需关注环形队列方案，但通过抽象层预留接口，
用户未来可以根据自己的场景自行实现最契合 I/O 行为的分配策略（如升序分配、连续块优先、
温度分层等），在编译期切换即可。

## 范围摘要

- `BlockAllocator` C++20 接口定义：6 个方法（Init / Acquire / Recycle / Available / Empty / RebuildFromActive）
- `RingBlockAllocator` 默认实现：固定大小环形队列（FIFO），容量 = block_count + 1（浪费一个槽位区分满/空）
- 目录结构：`slots/block_allocator.h`（接口）+ `slots/ring/ring_block_allocator.*`（实现）
- CMake 编译期切换：`CABE_BLOCK_ALLOCATOR=ring_queue`
- Engine 切换到块分配器抽象层，DeviceContext 持有 `BlockAllocatorImpl`
- TRIM 占位：Engine::Delete 路径预留 TRIM 调用点，当前不实际发送，P7 reactor 完善后做异步批量
- P5 恢复接口：`RebuildFromActive` 从已用块列表反推空闲块（P5M6 兑现并增补：越界/重复活块由静默处理升级为报错，见 P4.5M1 行为变更注）
- 删除旧代码：`engine/free_list.h` / `engine/free_list.cpp` / 相关测试
- 块分配器不持久化——恢复时从 MetaIndex 反推，纯内存操作（毫秒级）
- 为多设备打基础：每个 DeviceContext 独立持有自己的块分配器实例
- **不做**：异步 TRIM（P7）/ 性能基准（发版后补）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | 块分配器抽象层 + 环形队列实现 | `P4.5M1_block_allocator_design.md` | ✅ 已锁定（P4.5M3 收敛） |
| M2 | Engine 切换 + 旧代码清理 | `P4.5M2_engine_switch_design.md` | ✅ 已锁定（P4.5M3 收敛） |
| M3 | P4.5 收敛 | `P4.5M3_convergence_design.md` | ✅ 已锁定 |

## 里程碑依赖

```
P4.5M1 ──► P4.5M2 ──► P4.5M3
```

严格串行：抽象层 + 实现 → Engine 切换 → 收敛。

## 启动条件

1. ✅ P4 全部完成（io_uring 后端已实装）
2. ⏳ owner 确认启动
3. ⏳ 用 `/grill-with-docs P4.5M1` 开第一个里程碑的文档设计

## 已锁定决策

| 编号 | 决策 | 结果 |
|---|---|---|
| P4.5-D1 | TRIM 触发位置 | Engine 层（方案 B）——块分配器保持纯数据结构，不碰 I/O。当前占位不实现，P7 reactor 做异步批量 |
| P4.5-D2 | 里程碑划分 | 3 个里程碑（抽象层+实现 → Engine 切换 → 收敛）；目录 `slots/`，类名保持 `Block` 前缀 |
| P4.5-D3 | 方法命名 | `Acquire`（获取块号）/ `Recycle`（回收块号），取代 Allocate / Free，避免与内存操作混淆 |
| P4.5-D4 | 环形队列满/空判断 | 浪费一个槽位，容量 = block_count + 1，不做额外冗余 |
| P4.5-D5 | CMake 选项 | `CABE_BLOCK_ALLOCATOR=ring_queue` |

## 各里程碑范围

### P4.5M1（块分配器抽象层 + 环形队列实现）

**范围**：
- 新建 `slots/` 目录，定义 `BlockAllocator` C++20 接口（`slots/block_allocator.h`）
- 实现 `RingBlockAllocator`（`slots/ring/ring_block_allocator.*`）——固定大小环形队列（FIFO）
- `static_assert(BlockAllocator<RingBlockAllocator>)` 编译期验证
- 单元测试 + 契约测试
- `slots/CMakeLists.txt` 模块构建
- 根 `CMakeLists.txt` 加 `add_subdirectory(slots)` + `CABE_BLOCK_ALLOCATOR` 选项声明

**接口定义**（6 个方法）：

| 方法 | 说明 |
|---|---|
| `Init(dev, block_count)` | 首次打开——填入全部空闲块号（块 0 ~ block_count-1） |
| `Acquire(out)` | 获取一个可用块号 |
| `Recycle(id)` | 回收一个使用完的块号 |
| `Available()` | 当前可用块数 |
| `Empty()` | 是否无可用块 |
| `RebuildFromActive(dev, block_count, active_blocks)` | P5 恢复用——从已用块列表反推空闲块 |

**待梳理决策点**：
1. `Init` 是否需要考虑超级块（P5 第 0 块保留）——当前从块 0 开始，P5 改为从块 1 开始？

### P4.5M2（Engine 切换 + 旧代码清理）

**范围**：
- 新建 `engine/backend_config.h` 中的块分配器分派（`#if CABE_USE_BLOCK_RING_QUEUE`）
- `engine/CMakeLists.txt` 加 `CABE_BLOCK_ALLOCATOR` 编译期分派
- DeviceContext 改造：`FreeList free_list` → `BlockAllocatorImpl block_allocator`
- Engine 改造：`free_list.Allocate` → `block_allocator.Acquire`，`free_list.Free` → `block_allocator.Recycle`
- TRIM 占位：Engine::Delete 路径预留 TRIM 调用点（注释标记"P7 实现异步批量 TRIM"）
- 删除旧代码：`engine/free_list.h` / `engine/free_list.cpp` / `test/engine/free_list_test.cpp`
- `test/CMakeLists.txt` 移除旧测试目标

**待梳理决策点**：
1. Engine::Delete 中 TRIM 占位的具体形式——空函数？注释？预留待 TRIM 队列接口？

### P4.5M3（收敛）

**范围**：薄索引收敛稿 + 状态同步 + P5 占位索引。

## P4.5 退出条件概要

1. `BlockAllocator` 接口定义 + `RingBlockAllocator` 实装 + `static_assert` 通过
2. CMake `-DCABE_BLOCK_ALLOCATOR=ring_queue` 编译期分派生效
3. Engine 通过抽象层调用——全部测试不退步
4. 旧 `engine/free_list.*` 已删除
5. TRIM 占位就位（Delete 路径预留调用点）
6. P4.5M3 收敛稿审阅通过 + ROADMAP / README 状态同步

## 关键技术备忘（来自设计讨论）

以下内容来自 P4.5 设计讨论，在各里程碑的详细设计稿中展开：

1. **升序分配无性能价值**：1M 块大小下，升序但不连续的分配与完全随机分配在 NVMe SSD 上无可观测差异（< 1%）。
   块越大差距越小，4K 时约 5%~15%，1M 时趋近于零。
2. **空闲块号自然趋向无序**：随着 cabe 运行，空闲块号的回收顺序取决于用户删除行为，
   不可控。即使每轮排序，回收回来又乱——排序无法改变长期分布。
3. **FIFO 对 SSD 磨损更友好**：块使用更均匀，减少热块反复磨损。
4. **环形队列无锁扩展路径**：P7 阶段 `head` / `tail` 改为原子变量即可。
5. **TRIM 职责分离**：块分配器只管块号（纯数据结构），TRIM 由 Engine 层负责。
   当前占位，P7 利用 reactor 事件循环做异步批量发送。
6. **块分配器不持久化**：恢复时从 MetaIndex 反推——FreeList 重建是微秒到毫秒级纯内存操作，
   在整体恢复（WAL 重放为秒级磁盘 I/O）中占比可忽略。持久化的复杂度（一致性维护、额外 I/O）
   远大于节省的恢复时间。
7. **超级块（P5）**：P5 阶段每个设备的第 0 块作为超级块写入身份标识，届时 `Init` / `RebuildFromActive`
   需从块 1 开始分配，块 0 不进入空闲池。

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
