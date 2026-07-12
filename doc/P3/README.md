# P3 — IoBackend 与 MetaIndex 抽象层 · 设计文档索引

> P3 阶段目标：把 P1 写死的 I/O 路径和索引实现抽象为 C++20 concept，让后续阶段可以
> 替换后端（io_uring / SPDK / B+ 树等）而不改 Engine 代码。P3 当时实装
> `SyncIoBackend + HashMetaIndex`，功能与 P2 等价；P6 后 I/O 后端已取消构建默认值，必须显式选择。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P3 — IoBackend 与 MetaIndex 抽象层"。

## 状态

✅ **已完成**（P3M4 收敛通过）

## 范围摘要

- `IoBackend` C++20 concept：P3 时为同步裸缓冲区接口；P8M4 已增加
  `RegisterWriteBuffers(span<ValueBufferSlotView>)`，并将现行写接口升级为
  `Write(block_idx, const IoWriteBuffer&)`；读接口保持普通可写缓冲区
- `SyncIoBackend`：P3 当时的默认实现，包装 P1 的 pwrite / pread + O_DIRECT；P6 后仅作正确性回归
- `MetaIndex` C++20 concept：P3 时为 8 个方法（Insert / Lookup / Delete / Size / Contains / ForEach / WriteSnapshot / LoadSnapshot）
- `HashMetaIndex`：包装 P1 的 `unordered_map`；P5M4 已实装可中止 `ForEach`，并把 snapshot I/O 移出索引后端
- （P5M4 起 concept 收窄为现行 6 方法：移除 `WriteSnapshot` / `LoadSnapshot`，`ForEach` 改返回 `int32_t` 可中止——见 P5M4 设计稿）
- DeviceContext 改为持有抽象层实现
- Engine 通过 concept 接口调用（功能不变）
- CMake `CABE_IO_BACKEND` / `CABE_META_INDEX` 编译期分派生效
- **不做**：ValueBuffer（P8）/ 伪 SPDK（P9）/ 异步接口（P4）/ Snapshot 实装（P5）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | IoBackend 抽象层 | `P3M1_io_backend_design.md` | ✅ 已锁定（P3M4 收敛） |
| M2 | MetaIndex 抽象层 | `P3M2_meta_index_design.md` | ✅ 已锁定（P3M4 收敛） |
| M3 | Engine 切换 + CMake 分派 | `P3M3_engine_switch_design.md` | ✅ 已锁定（P3M4 收敛） |
| M4 | P3 收敛 | `P3M4_convergence_design.md` | ✅ 已锁定 |

## 里程碑依赖

```
P3M1 ──► P3M2 ──► P3M3 ──► P3M4
```

严格串行：IoBackend → MetaIndex → Engine 切换 → 收敛。

## 启动条件

1. ✅ P2 全部完成（API 冻结声明通过）
2. ✅ P3 决策 D1-D5 锁定
3. ✅ P3M1～P3M4 详细设计、实现与收敛全部完成

## 已知决策（已锁定）

| 编号 | 决策 | 结果 |
|---|---|---|
| P3-D1 | IoBackend 接口模型 | 同步 `Write` / `Read`；无 poll 模型；io_uring / SPDK 内部异步对 Engine 透明 |
| P3-D2 | `ValueBuffer` | 推到 P8；P3 继续用裸指针。P8 最终采用 value 专用 `ValueBuffer`，旧占位名 `BufferHandle` 废弃 |
| P3-D3 | MetaIndex concept 方法列表 | P3 时全部 8 个方法（含 ForEach / WriteSnapshot / LoadSnapshot 空壳）；P5M4 后为 6 个 |
| P3-D4 | 伪 SPDK / Mock | 不做；P3 只关注抽象层 + 默认实现 |
| P3-D5 | 里程碑划分 | 4 个 milestone（IoBackend → MetaIndex → Engine 切换 → 收敛） |

## 各里程碑设计前梳理的决策点（均已在对应设计稿锁定）

### P3M1（IoBackend 抽象层）

1. **IoBackend concept 的完整方法集**：除 Write / Read 外是否需要 `Open(path)` / `Close()` / `BlockCount()`？当前 Engine::Open 里直接 `::open()` + `ioctl(BLKGETSIZE64)`——是否移入 IoBackend？
2. **SyncIoBackend 的构造 / 生命周期**：构造时打开设备 vs 延迟到 Init 调用？
3. **concept 定义放哪个头文件**：`io/io_backend.h`（新目录 `io/`）还是 `engine/io_backend.h`（沿用 engine/）？

### P3M2（MetaIndex 抽象层）

4. **concept 定义放哪个头文件**：`index/meta_index_concept.h`（新目录 `index/`）还是 `engine/meta_index_concept.h`？
5. **契约测试套件形态**：模板化测试（`TYPED_TEST`）使任何 MetaIndex 实现都跑同一套用例？

### P3M3（Engine 切换）

6. **DeviceContext 模板化 vs 运行时多态**：DeviceContext 按 concept 模板参数化（编译期绑定）？还是用虚函数运行时分派？ROADMAP D20/D23 已锁定"编译期 dispatch"。
7. **Engine 是否变成模板类**：`Engine<IoBackendImpl, MetaIndexImpl>` 还是通过 CMake 条件编译（`#if CABE_IO_BACKEND == sync`）？

## P3 退出条件概要

1. IoBackend concept 定义 + SyncIoBackend 实装 + 单元测试
2. MetaIndex concept 定义 + HashMetaIndex 实装 + 契约测试
3. Engine 通过 concept 调用——65 个用例不退步 + 覆盖率 ≥ 80%
4. CMake `CABE_IO_BACKEND=sync` / `CABE_META_INDEX=hashmap` 编译期分派生效
5. P3M4 收敛稿审阅通过 + ROADMAP / README 状态同步

## 命名与目录约定

参见 [doc/P0/README.md](../P0/README.md) — 沿用 `P<阶段>M<里程碑>_<主题>_design.md`。
