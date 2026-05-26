# P4 — io_uring 后端 · 设计文档索引

> P4 阶段目标：实现 `IoUringIoBackend`，满足 P3 定义的 `IoBackend` C++20 concept，
> 启用 io_uring 的 registered buffers + FIXED ops + register_files。Engine 对外行为
> 不变（同步 API），内部由 io_uring 异步完成 I/O。P4 仍为单线程 + 单设备，多线程在 P7。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P4 — io_uring 后端"。

## 状态

🚧 **未启动**（P3 全部完成；待 owner 确认启动）

## 范围摘要

- `IoUringIoBackend` 完整实现：满足 `IoBackend` concept 的 5 个方法（Open / Close / BlockCount / Write / Read）
- liburing ≥ 2.9 接入（CMake `find_package` 优先，兜底策略待定）
- 每 `(device, reactor)` 一个独立 ring（P4 阶段 R=1，实际只有一个 ring）
- registered buffers 注册 + `IOSQE_FIXED_FILE` + `io_uring_register_files`
- submit / wait 模型（对 Engine 表现为同步——提交 SQE → 等待 CQE）
- TSAN 与 io_uring 的兼容性处理
- 部署文档：ulimit / RLIMIT_MEMLOCK / sysctl `kernel.io_uring_disabled`
- 性能基准：sync 后端 vs io_uring 后端对比，归档到 `bench/baselines/p4_io_uring.json`
- CMake `-DCABE_IO_BACKEND=io_uring` 编译期切换生效
- **不做**：多线程（P7）/ 零拷贝（P8）/ SPDK（P10）/ SQPOLL / DEFER_TASKRUN（P7 评估）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | liburing 接入 + 基础实现 | `P4M1_io_uring_basic_design.md` | 待设计 |
| M2 | 性能优化 | `P4M2_io_uring_optimize_design.md` | 待设计 |
| M3 | TSAN 兼容 + 部署文档 + 性能基准 | `P4M3_tsan_deploy_bench_design.md` | 待设计 |
| M4 | P4 收敛 | `P4M4_convergence_design.md` | 待设计 |

## 里程碑依赖

```
P4M1 ──► P4M2 ──► P4M3 ──► P4M4
```

严格串行：基础实现 → 性能优化 → 工程质量 → 收敛。

## 启动条件

1. ✅ P3 全部完成（IoBackend concept + CMake 分派已生效）
2. ⏳ owner 确认启动
3. ⏳ 用 `/grill-with-docs P4M1` 开第一个里程碑的文档设计

## 各里程碑范围与待梳理决策点

### P4M1（liburing 接入 + 基础实现）

**范围**：
- CMake 接入 liburing（`find_package` / `pkg-config` / `FetchContent` 策略待定）
- 新建 `io/uring/` 子目录，实现 `IoUringIoBackend`
- 基础 submit / wait 模型：每次 Write / Read 提交一个 SQE → `io_uring_submit` → `io_uring_wait_cqe` 等待单个 CQE
- `static_assert(IoBackend<IoUringIoBackend>)` 编译期验证
- CMake `CABE_IO_BACKEND=io_uring` 分派——`engine/backend_config.h` 加 `#elif` 分支
- 单元测试：复用 SyncIoBackend 的测试结构（需 loop 设备）
- `engine/CMakeLists.txt` 加 `elseif(CABE_IO_BACKEND STREQUAL "io_uring")` 分支

**待梳理决策点**：
1. liburing 接入方式：系统库 `pkg-config` 优先 + `FetchContent` 兜底？还是只依赖系统库？
2. ring 大小（队列深度）：固定值（如 64 / 128）还是可配置？
3. Open 时机：ring 在 `Open(path)` 内初始化还是构造时？
4. O_DIRECT 是否仍然需要：io_uring 可以与 O_DIRECT 配合，是否保持？
5. 错误码映射：io_uring CQE 的负错误码如何映射到 cabe 的 `err::kIoBase` 段？

### P4M2（性能优化）

**范围**：
- registered buffers：`io_uring_register_buffers` 预注册 BufferPool 的对齐 buffer
- `IOSQE_FIXED_FILE`：`io_uring_register_files` 预注册 fd
- Write / Read 切换到 `IORING_OP_WRITE_FIXED` / `IORING_OP_READ_FIXED`
- 评估是否需要批量提交（当前 P4 为单线程同步模型，批量提交收益有限）

**待梳理决策点**：
1. registered buffers 的 buffer 来源：直接复用 `BufferPool` 还是 `IoUringIoBackend` 自管？
2. BufferPool 是否需要暴露 buffer 地址列表接口？
3. 注册时机：Open 时一次性注册还是延迟注册？
4. 注册失败处理：RLIMIT_MEMLOCK 不够时如何降级？

### P4M3（TSAN 兼容 + 部署文档 + 性能基准）

**范围**：
- TSAN 与 io_uring 的兼容性：内核态完成的 I/O 对 TSAN 不可见，可能报假阳性
- 部署文档：ulimit -l（RLIMIT_MEMLOCK）/ sysctl `kernel.io_uring_disabled` / 所需内核版本
- 性能对比基准：sync 后端 vs io_uring 后端，在 loop 设备上对比 Put / Get 延迟和吞吐
- 基准归档到 `bench/baselines/p4_io_uring.json`

**待梳理决策点**：
1. TSAN 兼容策略：TSAN 构建时禁用 io_uring（回退到 sync 后端）？还是插入 TSAN 注解？
2. 性能基准的测试设备：loop 设备还是真实 NVMe？
3. 性能回归红线：ROADMAP 未给 P4 定红线——是否需要？

### P4M4（收敛）

**范围**：薄索引收敛稿 + 状态同步 + P4.5 占位索引。与 P3M4 对称。

## P4 退出条件概要

1. `IoUringIoBackend` 实装 + `static_assert(IoBackend<IoUringIoBackend>)` 通过
2. CMake `-DCABE_IO_BACKEND=io_uring` 编译、链接、测试全绿
3. registered buffers + `IOSQE_FIXED_FILE` 启用
4. TSAN 兼容方案落地（四档全绿或 TSAN + io_uring 组合有明确的处理策略）
5. 部署文档就位
6. 性能基准归档（sync vs io_uring 对比）
7. P4M4 收敛稿审阅通过 + ROADMAP / README 状态同步

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
