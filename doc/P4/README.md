# P4 — io_uring 后端 · 设计文档索引

> P4 阶段目标：实现 `IoUringIoBackend`，满足 P3 定义的 `IoBackend` C++20 concept，
> 启用 io_uring 的预注册文件描述符（registered files）。Engine 对外行为
> 不变（同步 API），内部由 io_uring 异步完成 I/O。P4 仍为单线程 + 单设备，多线程在 P7。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P4 — io_uring 后端"。

## 状态

✅ **已完成**（P4M1～P4M4 已实装并由 P4M4 收敛）

> 后续兑现：P7 在该后端上完成 reactor / 多线程 / 多设备框架迁移，但 io_uring 仍采用提交即等待；
> P8 完成 registered buffers。P9 将 SPDK 作为长期 I/O 方向，io_uring 保留为过渡与回归后端。
> P6 起所有测试脚本都必须显式传入 `--backend`。

## 范围摘要

- `IoUringIoBackend` 在 P4 完成当时的 5 方法接口；P8M4 后已适配新增的
  `RegisterWriteBuffers` 和 `IoWriteBuffer` 写入协议
- liburing ≥ 2.9 硬性系统依赖（CMake `pkg_check_modules` 校验版本）
- 每 `(device, reactor)` 一个独立 ring（P4 阶段 R=1，实际只有一个 ring）
- 预注册文件描述符（`io_uring_register_files` + `IOSQE_FIXED_FILE`）
- 最简提交/等待模型（对 Engine 表现为同步——提交 SQE → 等待 CQE）
- TSAN 与 io_uring 的兼容性处理（维持互斥检查 + 文档说明）
- 部署文档：ulimit / RLIMIT_MEMLOCK / sysctl `kernel.io_uring_disabled`
- 测试脚本改进：`--device=` 参数化传入设备路径
- CMake `-DCABE_IO_BACKEND=io_uring` 编译期切换生效
- **不做**：预注册缓冲区（P8 已兑现）/ P4 独立性能基准（首个正式锚点在 P6）/ 多线程（P7 已兑现）/ 零拷贝（P8 已兑现）/ SPDK（P9 正在推进）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | liburing 接入 + 基础实现 | `P4M1_io_uring_basic_design.md` | ✅ 已锁定（P4M4 收敛） |
| M2 | 预注册文件描述符优化 | `P4M2_io_uring_optimize_design.md` | ✅ 已锁定（P4M4 收敛） |
| M3 | TSAN 兼容 + 部署文档 + 脚本改进 | `P4M3_tsan_deploy_design.md` | ✅ 已锁定（P4M4 收敛） |
| M4 | P4 收敛 | `P4M4_convergence_design.md` | ✅ 已锁定 |

## 里程碑依赖

```
P4M1 ──► P4M2 ──► P4M3 ──► P4M4
```

严格串行：基础实现 → 性能优化 → 工程质量 → 收敛。

## 启动条件

1. ✅ P3 全部完成（IoBackend concept + CMake 分派已生效）
2. ✅ owner 已确认启动
3. ✅ P4M1～P4M4 详细设计、实现与收敛全部完成

## 各里程碑范围与设计前决策点（均已锁定）

### P4M1（liburing 接入 + 基础实现）

**范围**：
- CMake 通过 `pkg-config` 接入系统 `liburing >= 2.9`；版本不满足时直接配置失败，不做 `FetchContent` 降级
- 新建 `io/uring/` 子目录，实现 `IoUringIoBackend`
- 基础 submit / wait 模型：每次 Write / Read 提交一个 SQE → `io_uring_submit` → `io_uring_wait_cqe` 等待单个 CQE
- `static_assert(IoBackend<IoUringIoBackend>)` 编译期验证
- CMake `CABE_IO_BACKEND=io_uring` 分派——`engine/backend_config.h` 加 `#elif` 分支
- 单元测试：复用 SyncIoBackend 的测试结构（需 loop 设备）
- `engine/CMakeLists.txt` 加 `elseif(CABE_IO_BACKEND STREQUAL "io_uring")` 分支

**设计前问题（答案见 P4M1 详细稿）**：
1. liburing 接入方式：系统库 `pkg-config` 优先 + `FetchContent` 兜底？还是只依赖系统库？
2. ring 大小（队列深度）：固定值（如 64 / 128）还是可配置？
3. Open 时机：ring 在 `Open(path)` 内初始化还是构造时？
4. O_DIRECT 是否仍然需要：io_uring 可以与 O_DIRECT 配合，是否保持？
5. 错误码映射：io_uring CQE 的负错误码如何映射到 cabe 的 `err::kIoBase` 段？

### P4M2（预注册文件描述符优化）

**范围**：
- 预注册文件描述符（`io_uring_register_files`）：Open 时注册 fd，Write / Read 使用固定文件标志（`IOSQE_FIXED_FILE`）
- 预注册缓冲区在 P4 时推到 P8；P8M4 后已通过 `RegisterWriteBuffers`、`IoWriteBuffer` 和槽位身份完成实现

**已锁定决策**：
1. P4M2-D1：只做预注册文件描述符，预注册缓冲区推到 P8
2. P4M2-D2：Open 内 ring 初始化后立即注册，Close 时先注销
3. P4M2-D3：注册失败统一返回错误，不做降级

### P4M3（TSAN 兼容 + 部署文档 + 脚本改进）

**范围**：
- TSAN 兼容：维持现有互斥检查（`run-tests.sh` 拒绝 io_uring + TSAN 组合）+ 部署文档说明
- 部署文档：io_uring 的系统要求（ulimit / RLIMIT_MEMLOCK / sysctl / 内核版本）
- 脚本改进：`run-tests.sh` / `run-coverage.sh` 加 `--device=` 参数，取代环境变量传入设备路径
- P5 超级块方案记录

**已锁定决策**：
1. P4M3-D1：TSAN 维持现状——互斥检查 + 文档说明
2. P4M3-D2：测试设备通过 `--device=` 参数传入
3. P4M3-D3：P4 不做性能基准；P6 随后建立首个正式 io_uring 历史锚点

### P4M4（收敛）

**范围**：薄索引收敛稿 + 状态同步 + P4.5 占位索引。与 P3M4 对称。

## P4 退出条件概要

1. `IoUringIoBackend` 实装 + `static_assert(IoBackend<IoUringIoBackend>)` 通过
2. CMake `-DCABE_IO_BACKEND=io_uring` 编译、链接、测试全绿
3. 预注册文件描述符（`IOSQE_FIXED_FILE`）启用
4. TSAN 兼容方案落地（互斥检查 + 文档说明）
5. 部署文档就位
6. 测试脚本支持 `--device=` 参数化传入设备
7. P4M4 收敛稿审阅通过 + ROADMAP / README 状态同步

## 命名与目录约定

沿用 `P<阶段>M<里程碑>_<主题>_design.md`。参见 [doc/P0/README.md](../P0/README.md)。
