# P4 — io_uring 后端 · 设计文档索引

> P4 阶段目标：实现 `IoUringIoBackend`，满足 P3 定义的 `IoBackend` C++20 concept，
> 启用 io_uring 的预注册文件描述符（registered files）。Engine 对外行为
> 不变（同步 API），内部由 io_uring 异步完成 I/O。P4 仍为单线程 + 单设备，多线程在 P7。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P4 — io_uring 后端"。

## 状态

🚧 **未启动**（P3 全部完成；待 owner 确认启动）

## 范围摘要

- `IoUringIoBackend` 完整实现：满足 `IoBackend` concept 的 5 个方法（Open / Close / BlockCount / Write / Read）
- liburing ≥ 2.9 硬性系统依赖（CMake `pkg_check_modules` 校验版本）
- 每 `(device, reactor)` 一个独立 ring（P4 阶段 R=1，实际只有一个 ring）
- 预注册文件描述符（`io_uring_register_files` + `IOSQE_FIXED_FILE`）
- 最简提交/等待模型（对 Engine 表现为同步——提交 SQE → 等待 CQE）
- TSAN 与 io_uring 的兼容性处理（维持互斥检查 + 文档说明）
- 部署文档：ulimit / RLIMIT_MEMLOCK / sysctl `kernel.io_uring_disabled`
- 测试脚本改进：`--device=` 参数化传入设备路径
- CMake `-DCABE_IO_BACKEND=io_uring` 编译期切换生效
- **不做**：预注册缓冲区（P8）/ 性能基准（发版后补）/ 多线程（P7）/ 零拷贝（P8）/ SPDK（P10）

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | liburing 接入 + 基础实现 | `P4M1_io_uring_basic_design.md` | 待设计 |
| M2 | 预注册文件描述符优化 | `P4M2_io_uring_optimize_design.md` | 待设计 |
| M3 | TSAN 兼容 + 部署文档 + 脚本改进 | `P4M3_tsan_deploy_design.md` | 待设计 |
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

### P4M2（预注册文件描述符优化）

**范围**：
- 预注册文件描述符（`io_uring_register_files`）：Open 时注册 fd，Write / Read 使用固定文件标志（`IOSQE_FIXED_FILE`）
- 预注册缓冲区（registered buffers）推到 P8——当前 IoBackend 接口不传缓冲区索引，P8 重新设计缓冲区管理时统一处理

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
3. P4M3-D3：性能基准发版后再补，P4 不做

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
