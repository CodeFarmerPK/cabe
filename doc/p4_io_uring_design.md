# P4 io_uring 分阶段实施设计

> **状态**:草稿 v1.0(讨论稿) · 2026-04-28
> **作者**:CodeFarmerPK + Claude(Opus 4.7, 1M context)
> **基线**:P3 已交付的 IoBackend 抽象层(`io/io_backend.h` + `io/buffer_handle.{h,cpp}` + `io/backends/sync_*`)
> **相关文档**:
> - `README.md` — 项目总览、Quick Start、Roadmap 表
> - `memory/project_roadmap.md` — 各阶段完成情况与架构决策
> - `memory/feedback_engineering_principle.md` — 先落地后优化
> - `memory/feedback_robust_runtime_assumption.md` — 健壮运行环境假设

---

## 0. 文档元信息

### 0.1 状态约定

| 标签 | 含义 |
|---|---|
| ✅ 已定 | 与用户讨论确认,落地阶段不再翻案 |
| 🔍 待评测 | 决策已定但具体参数(如 SQ depth)需 bench 验证后微调 |
| 📌 待议 | 文档列出选项,P4 启动前需明确选择 |
| ⏭ 推后 | 明确不在 P4 范围内,推到后续阶段 |

### 0.2 版本与依赖

| 项 | 要求 |
|---|---|
| 平台 | Fedora 43(`CMakeLists.txt:31` 强校验) |
| 内核 | 6.16+(Fedora 43 默认) |
| 编译器 | GCC 15+ / Clang 20+ |
| liburing | `>= 2.9-2`(D12) |
| 链接方式 | 动态(D11) |
| C++ 标准 | C++20 |

---

## 1. 背景与目标

### 1.1 P4 在路线图中的位置

```
P-1 ✅ Fedora 43 开发环境
P0  ✅ BufferPool(mmap + O_DIRECT 对齐)
P1  ✅ 线程安全(shared_mutex + atomic)+ Google Benchmark 骨架
P2  ✅ C++ API 契约定型(Pimpl + Status)
P3  ✅ IoBackend 抽象层(编译期 dispatch,sync 后端)
P4  🚧 io_uring 后端 + registered buffer pool        ← 本文档
P5  WAL + 崩溃恢复
P6  多线程 reactor 引擎
P7  自研 B+ 树 + 细粒度并发
P8  scatter-gather 多 chunk 合并 I/O
P9  SPDK 用户态驱动后端(可选)
```

### 1.2 P4 为什么是 io_uring

P3 抽象层落地时已经把 io_uring 作为 P4 的明确对象。具体动机:

1. **兑现 P3 Q2 零拷贝路线**
   P3 把 AcquireBuffer 的契约从"清零"改为"内容未定义",理由是为零拷贝 I/O 路线让路。
   io_uring 的 `IORING_REGISTER_BUFFERS` + `*_FIXED` ops 是这条路线的第一个落地点。

2. **消除 GUP / fdget 每次 I/O 的固定开销**
   sync 路径的 `pwrite` 每次都要走 get_user_pages + fdget。1 MiB chunk 上这部分占比小,
   但当并发上来 / 块更小时会显著拖慢。registered buffers + registered files 一次性付清。

3. **解锁深队列 I/O**
   sync 路径的 `pwrite` 是同步阻塞,对 16 chunk 大 value 要串行 16 次 syscall。
   io_uring 允许一次提交多个 SQE,内核 / 硬件可以并行调度。

4. **为 P6 reactor 留生态**
   reactor 引擎需要异步 I/O 引擎作底座。io_uring 是 Linux 当前唯一成熟的真异步块 I/O 路径。

### 1.3 P4 与 sync → epoll → io_uring 路线的关系

不走 epoll 中间步。理由(完整论述见跨 LLM 讨论笔记,简述如下):

- epoll 是 fd readiness 通知机制,只对"有 ready/not-ready 状态"的 fd 有意义(socket、pipe、eventfd)。
- 常规文件 / 块设备永远是 "ready" 状态,epoll 加块设备 fd 永远立刻返回 readable,I/O 该多慢还多慢。
- 块 I/O 真正异步路径:POSIX AIO(实质 userspace 线程池,弃用)、Linux AIO(libaio,已被 io_uring 取代且维护停滞)、io_uring。

**结论**:Cabe 直接走 sync → io_uring,跳过 epoll。

---

## 2. 范围声明

### 2.1 In-scope

| 项 | 描述 |
|---|---|
| 新增 `IoUringIoBackend` 类 | 满足 `IoBackendTraits` concept,通过 `io/io_backend.h` 已有的编译期 dispatch 选用 |
| `io_uring_register_buffers` + `WRITE_FIXED`/`READ_FIXED` | P3 BufferPool 的 mmap 区段一次性注册为 io_uring 的 fixed buffer |
| `io_uring_register_files` + `IOSQE_FIXED_FILE` | 设备 fd 注册为 ring 的 fixed file |
| 内部批量 API `WriteBlocks/ReadBlocks` | M7,可选地在 P4 内交付,公开 API 不变 |
| Close 期间 drain 未完成 CQE 的契约 | 与 P3 Q7 outstanding handle 检查并存 |
| CMake `CABE_IO_BACKEND=io_uring` 取消 FATAL_ERROR | 真实链接 liburing |
| TSAN + io_uring 编译期阻断 | CMake + scripts 双层 |
| bench 基线归档 | `p4-pre-fixed` / `p4-post-fixed` / `p4-post-batch` 三档 |

### 2.2 Out-of-scope(明确推到后续阶段)

| 项 | 推到 | 理由 |
|---|---|---|
| Engine 公开 API 异步化(`Put`/`Get` 阻塞 → callback / future) | P6 | 公开 API 在 P3 刚冻结,P4 不翻;async 是 reactor 的天然产物 |
| value 真正的端到端零拷贝 | P7 + P8 | 需要 metadata 模型重设计 + scatter-gather 写入 + B+ 树就位 |
| SQPOLL / IOPOLL | P5/P6 单独评估 | 改变 reaping 模型,与 reactor 设计耦合 |
| 多 NVMe 拆分(`vector<IoBackend>`) | P4+ 跨阶段 | P4 内仍单 IoBackend 实例;轮转分配 ChunkId→Device 是更高层议题 |
| `uring_cmd` / NVMe 直通 | P9 | 与 SPDK 路线竞争同一价值 |

---

## 3. 核心架构决策汇总

| ID | 决策 | 状态 | 详细见 |
|---|---|---|---|
| D1 | 用 liburing,不裸 syscall | ✅ | §4.2 |
| D2 | 1 ring / IoBackend 实例 | ✅ | §5 |
| D3 | P4 公开 API 保持 sync | ✅ | §1.2、§12 |
| D4 | 保守起步:M3 朴素 → M4 FIXED → M5 register_files | ✅ | §13 |
| D5 | 静态 buffer 注册(Open 时一次性) | ✅ | §6.1 |
| D6 | SQPOLL / IOPOLL 不进 P4 | ✅ | §2.2 |
| D7 | SQ/CQ depth 默认 64,做成 Options 字段 | 🔍 | §6.4 |
| D8 | 错误码沿用 `IO_BACKEND_*` 七种 | ✅ | §10 |
| D9 | Close 必须 drain in-flight CQE | ✅ | §9 |
| D10 | feature gate 用 `CABE_HAVE_*` | ✅ | §17 |
| D11 | 动态链接 liburing | ✅ | §4.2 |
| D12 | `liburing >= 2.9` | ✅ | §4.2 |
| D13 | n × 1 MiB iovec 注册;value 零拷贝是 P5/P7/P8 联动议题 | ✅ | §6.2、§6.5 |
| D14 | Model A 起步;M7 后评估 B | ✅ | §8.2 |
| D15 | register_buffers 失败 → Open 整体失败 | ✅ | §6.3 |
| D16 | user_data 三档演进(0 → 数组下标 → Request*) | ✅ | §8.3 |
| D17 | drain 无限等(健壮运行环境假设) | ✅ | §9.3 |
| D18 | M7 在 P4 收尾阶段引入,时机灵活 | ✅ | §13(M7) |
| D19 | CMake + scripts 双层阻断 `io_uring + TSAN` | ✅ | §11 |

---

## 4. 与现有代码的衔接

### 4.1 不变的部分

| 项 | 文件 | 锁定原因 |
|---|---|---|
| `IoBackendTraits` concept | `io/io_backend.h:57` | 七个方法签名是 sync 与 io_uring 的共契约 |
| `BufferHandle` 公开形态 | `io/buffer_handle.h` | PIMPL,只暴露 `valid() / data() / size()` |
| `Engine` 公开 API | `include/cabe/engine.h` | P3 已冻结 |
| `engine_api.cpp::TranslateStatus` | `engine/engine_api.cpp` | `IO_BACKEND_*` 7 条 case 已就位,P4 无需新增 |
| `cabe::Options` 字段语义 | `include/cabe/options.h` | `buffer_pool_count` 维持 slot 语义(P3 Q4) |
| 编译期 dispatch 机制 | `CMakeLists.txt:167-191` + `CMakePresets.json` + `scripts/run-tests.sh:37-38` | 框架已就位 |

### 4.2 新增的部分

| 项 | 路径 | 内容 |
|---|---|---|
| io_uring 后端实现 | `io/backends/io_uring_io_backend.{h,cpp}` | 类形态镜像 `sync_io_backend.{h,cpp}` |
| io_uring BufferHandleImpl | `io/backends/io_uring_buffer_handle_impl.h` | 字段比 sync 多一个 `fixed_buf_index_` |
| liburing 依赖 | `CMakeLists.txt`:`pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.9)`(D11、D12) | 动态链接 |
| CMake io_uring 分支落地 | `CMakeLists.txt:172-180` | 取消 FATAL_ERROR;`target_sources` + `target_link_libraries(cabe_lib PUBLIC PkgConfig::LIBURING)` |
| TSAN 阻断检查 | `CMakeLists.txt`(放在 `CABE_IO_BACKEND` 与 `CABE_ENABLE_TSAN` 都解析完之后) | FATAL_ERROR(D19) |
| io_uring CMake preset | `CMakePresets.json` | 新增 `io_uring-debug` / `io_uring-release` / `io_uring-asan`;现有 `io_uring-release` 取消 "currently FATAL_ERROR" 描述 |
| io_uring 专属测试 | `test/io/io_uring_specific_test.cpp` | RegisterBuffers 失败 / drain / 等 io_uring 独有行为 |
| bench 基线 | `bench/baselines/p4-pre-fixed-*.json` / `p4-post-fixed-*.json` / `p4-post-batch-*.json` | M3、M5、M7 各归档一次 |

### 4.3 小改的部分

| 项 | 改动 |
|---|---|
| `io/buffer_handle.h:71-75` | `friend class IoUringIoBackend` 已经预置(P3 M1 写好),不需要再改 |
| `io/buffer_handle.cpp:18-22` | 同上,`#include "io/backends/io_uring_buffer_handle_impl.h"` 已就位 |
| `io/io_backend.h:40-44` | 同上,`using IoBackend = IoUringIoBackend` 类型别名已就位 |
| `scripts/run-tests.sh` | TSAN + io_uring 组合提前 reject(double-block) |
| `scripts/run-bench.sh` | 同上 |
| `README.md` Roadmap 表 | P4 进度跟随 milestone 推进 |
| `memory/project_roadmap.md` | 完工后追加"P4 实现摘要" |

---

## 5. Ring 拓扑(D2)

### 5.1 P4 选择:1 ring / IoBackend 实例

```
┌─────────────────────────────────────────────────┐
│ Engine                                          │
│                                                 │
│  ┌───────────────────────────────────────────┐  │
│  │ IoUringIoBackend                          │  │
│  │                                           │  │
│  │  fd_(已注册)                              │  │
│  │  ring_                                    │  │
│  │  pool_base_(mmap,已 register_buffers)    │  │
│  │  freeStack_                               │  │
│  │  outstanding_count_(P3 Q7)                │  │
│  │  in_flight_count_(P4 新增)                │  │
│  │  io_mutex_(Model A)                       │  │
│  └───────────────────────────────────────────┘  │
└─────────────────────────────────────────────────┘
```

### 5.2 演进路径

| 阶段 | Ring 拓扑 |
|---|---|
| **P4** | 1 ring / IoBackend 实例 |
| P4+ 多 NVMe(跨阶段) | 1 ring / device(每个设备一个 IoBackend 实例) |
| P6 reactor | 1 ring / reactor 线程 |
| P7+ | 与 B+ 树并发模型联动 |

P4 不为 P6 提前抽象 ring 数量,但 IoUringIoBackend 内部成员设计要保证"换 ring 拓扑不破坏 IoBackend 公开 API"。这本来就是 P3 抽象层的承诺,P4 只是不退化它。

---

## 6. Buffer 注册模型(D5、D13、D15)

### 6.1 注册时机

```
Open(path, count):
  1. open(path, O_DIRECT|O_SYNC|O_RDWR) → fd_
  2. fstat / S_ISBLK / ioctl(BLKGETSIZE64) 校验(沿用 sync 后端逻辑)
  3. mmap(MAP_ANONYMOUS, count × 1 MiB) → pool_base_
  4. 切片 + 填 freeStack_
  5. io_uring_queue_init(SQ_DEPTH, &ring_)
  6. io_uring_register_buffers(ring_, iovecs, count)        ← D13:n × 1 MiB iovec
     失败 → 走 §6.3 错误处理(D15)
  7. io_uring_register_files(ring_, &fd_, 1)
  8. opened_ = true
```

**反向次序**(`Close`):

```
Close():
  1. closed_.store(true)                 ← 拒绝新提交
  2. drain(in_flight_count_ → 0)         ← §9 详解
  3. Q7 outstanding handle 检查          ← 沿用 P3
  4. io_uring_unregister_files(ring_)
  5. io_uring_unregister_buffers(ring_)
  6. io_uring_queue_exit(ring_)
  7. munmap(pool_base_)
  8. close(fd_)
```

### 6.2 注册粒度(D13)

**采用 n × 1 MiB iovec**,与 P3 BufferPool 的 slot 模型一一对应。

```
mmap 一片 (n × 1 MiB):
  ┌──────┬──────┬──────┬──────┬──────┐
  │ slot0│ slot1│ slot2│ ...  │slot n-1
  └──────┴──────┴──────┴──────┴──────┘
   ↑      ↑      ↑      ↑      ↑
   注册为 n 个独立的 iovec:
   iovecs[0] = {slot0, 1 MiB}
   iovecs[1] = {slot1, 1 MiB}
   ...
   iovecs[n-1] = {slot(n-1), 1 MiB}

io_uring_register_buffers(ring, iovecs, n)
   → fixed_buf_index 0..n-1
   → 与 freeStack_ 的 slot_index 一一对应
```

`BufferHandleImpl` 字段差异:

| 字段 | sync(`io/backends/sync_buffer_handle_impl.h`) | io_uring(新) |
|---|---|---|
| `ptr_` | mmap 区段内指针 | 同 |
| `size_` | 1 MiB | 同 |
| `slot_index_` | freeStack_ 槽号 | 同 |
| `owner_` | `SyncIoBackend*` | `IoUringIoBackend*` |
| `fixed_buf_index_` | — | == slot_index_(直接复用) |

**为什么选 n × iovec 而不是单个大 iovec**:
- `WRITE_FIXED` / `READ_FIXED` 要求 buffer 是 iovec 整体,不能写 iovec 子段(否则 op 类型要换)
- n × iovec 让 `fixed_buf_index == slot_index`,代码无额外映射逻辑
- 单 iovec 方案要求每次 op 携带 offset,反而复杂

### 6.3 注册失败处理(D15)

**采用 A:Open 整体失败,不 fallback。**

```
Open(...):
  ...到 io_uring_register_buffers(ring, iovecs, count)
  
  if (ret < 0) {
      io_uring_queue_exit(ring_);
      munmap(pool_base_);
      close(fd_);
      return IO_BACKEND_NOT_OPEN;       // 或更具体的错误码
  }
```

**触发场景与处置**:

| 触发 | 用户侧动作 |
|---|---|
| `RLIMIT_MEMLOCK` 撞限(最常见) | `ulimit -l unlimited` 或 systemd `LimitMEMLOCK=infinity` |
| 系统 memory pressure / OOM | 减小 `Options.buffer_pool_count` 重试 |
| Hardened kernel / Docker seccomp 禁 io_uring | 切回 `--backend=sync` |

**为什么不 fallback 到非 FIXED**:
1. fallback 会让用户在不知情的情况下损失零拷贝路径的核心收益,违背"推到硬件极限"产品方向
2. 双路径维护(FIXED 和非 FIXED)成倍增加 backend 内部代码量
3. 失败信息明确,用户修一次 ulimit 永久解决

**README 部署文档**(P4 收尾阶段补):

> ### Production deployment notes
>
> Cabe io_uring backend requires non-trivial RLIMIT_MEMLOCK to register buffers.
> For a `buffer_pool_count` of N (default 16), you need at least N MiB locked memory.
>
> ```bash
> ulimit -l unlimited        # interactive
> # OR systemd:
> [Service]
> LimitMEMLOCK=infinity
> ```

### 6.4 SQ / CQ Depth(D7)

| 项 | 值 |
|---|---|
| 默认 SQ depth | 64 |
| 默认 CQ depth | SQ depth × 2 = 128(io_uring 默认行为) |
| 配置入口 | 新增 `Options.io_uring_sq_depth`(`include/cabe/options.h`),默认 64 |
| 实测节奏 | 🔍 M5 完成后跑 1 / 8 / 64 / 256 四档 sweep,根据数据微调默认值 |

**为什么不强制单一值**:
- `Options.buffer_pool_count` 控制并发持有 buffer 的上限(slot 语义,P3 Q4 锁定)
- `Options.io_uring_sq_depth` 控制 in-flight I/O 上限
- 两者独立:你可能 pool_count=16 / sq_depth=64(同一 slot 不能多个 in-flight,但不同 slot 可以)

  ⚠ 实际上 sq_depth 不应小于 pool_count,因为最坏情况 n 个 buffer 同时 in-flight。
  约束:`sq_depth >= buffer_pool_count`,Open 校验。

### 6.5 value 零拷贝长期路线(术语澄清)

P4 不动 value-staging buffer 形态。本节澄清长期方向,避免后续阶段误解。

#### 术语区分

| 术语 | 含义 | Cabe 是否采用 |
|---|---|---|
| **inline block header** | 每个 1 MiB 盘上块前缀塞几 KiB 的 magic / CRC / chunk_id | 暂未,可选,P5+ 决定 |
| **centralized metadata region** | 盘上独立元数据区(`metadata region` + `data region` 双区) | 长期方向 |
| **header**(松散用法) | 之前讨论中"块头"含义,会与上面两者混淆 | **不再使用此词** |
| **metadata** | 中心化元数据,RAM 权威 + 周期检查点 | 长期方向 |

#### 长期数据布局(P5–P8 联动落地)

```
裸设备布局:
┌──────────────────┬───────────────────────────────┐
│ metadata region  │  data region                  │
│ (头部固定区)      │  纯 value 字节,1 MiB 对齐    │
│                  │                               │
│ ChunkMeta[0]     │  block 0: pure user data     │
│ ChunkMeta[1]     │  block 1: pure user data     │
│ ChunkMeta[2]     │  ...                          │
│ ...              │                               │
└──────────────────┴───────────────────────────────┘

内存中:
ChunkIndex(map<ChunkId, ChunkMeta>)  ← 最新、权威
                ↓ 周期性 flush
metadata region(盘上)                ← 检查点

WAL(P5 引入)                       ← 填补两次 flush 之间的窗口
```

#### buffer pool 在长期形态下服务的对象

| 数据类型 | 走 buffer pool? | P4 状态 |
|---|---|---|
| value 主体(用户数据) | 长期不走(零拷贝) | P4 仍走(staging copy) |
| metadata(CRC、timestamp、key meta) | 走 | P4 不动(在 RAM ChunkIndex 里) |
| 索引页(B+ 树节点、hashmap 桶) | 走 | P4 不动(B+ 树在 P7) |
| WAL 记录 | 走 | P4 不动(WAL 在 P5) |
| 小 value 的 padding 兜底 | 可能走 | P4 仍走(Engine::Put 小 value 分支) |

**P4 该做的事**:让当前 value-staging buffer 享受 io_uring registered + FIXED ops。
**P4 不该做的事**:重新设计盘上格式或 metadata 模型。

---

## 7. File 注册模型

### 7.1 设计

```
Open(...) 的步骤 7:
  io_uring_register_files(ring_, &fd_, 1)

WriteBlock / ReadBlock 提交时:
  sqe = io_uring_get_sqe(ring_)
  io_uring_prep_write_fixed(sqe, fd_idx=0, buf_ptr, size, offset, fixed_buf_idx)
  sqe->flags |= IOSQE_FIXED_FILE      ← 用 registered file index 而不是 fd
```

### 7.2 收益

每次 op 跳过 kernel 侧的 `fdget` / `fdput`,大约几十 ns。占比小但是常态化收益,合理。

### 7.3 何时反注册

`Close()` 内,在 `unregister_buffers` 之后、`queue_exit` 之前。次序见 §6.1。

---

## 8. 提交/完成模型(D14、D16)

### 8.1 多并发提交背景

Engine 的并发图:

| 路径 | Engine 锁层级 | IoBackend 看到的并发 |
|---|---|---|
| `Put/Delete` | `unique_lock`(写) | 1 |
| `Get` | `shared_lock`(读) | ≥ 1(多 Get 同时读不同 key) |
| `Open/Close/Remove` | `unique_lock`(写) | 1 |

所以 IoUringIoBackend 必须能正确处理**读路径上的多线程并发提交**。

### 8.2 Model A 详解(D14)

**Model A:io_mutex_ 包住 submit + wait 全程**

```
WriteBlock(blockId, handle):
  if (closed_.load()) return IO_BACKEND_NOT_OPEN;
  if (!handle.valid())  return IO_BACKEND_INVALID_HANDLE;
  
  std::lock_guard lk(io_mutex_);
  in_flight_count_++;
  
  sqe = io_uring_get_sqe(ring_);
  io_uring_prep_write_fixed(sqe, /*fd_idx=*/0, handle.data(), CABE_VALUE_DATA_SIZE,
                            blockId * CABE_VALUE_DATA_SIZE,
                            handle.fixed_buf_index());
  sqe->flags |= IOSQE_FIXED_FILE;
  // sqe->user_data = 0;   // M3-M6 不使用,Model A 1:1 不需要 (D16)
  
  ret = io_uring_submit_and_wait(ring_, 1);
  if (ret < 0) {
      in_flight_count_--;
      return IO_BACKEND_SUBMIT_FAILED;
  }
  
  io_uring_peek_cqe(ring_, &cqe);
  result = cqe->res;
  io_uring_cqe_seen(ring_, cqe);
  in_flight_count_--;
  
  if (result < 0)                    return IO_BACKEND_IO_FAILED;
  if (result != CABE_VALUE_DATA_SIZE) return IO_BACKEND_IO_FAILED;
  return SUCCESS;
```

**特性**:
- 实际 I/O 并发 = 1(`io_mutex_` 串行)
- 不需要 `user_data` 关联(刚提交的 cqe 必然是自己的)
- 不需要 reaper 线程
- 收益来源:registered buffers(GUP 消除)+ registered files(fdget 消除)+ syscall 减少

**性能上限**:对单线程 / 顺序 I/O 已经接近最优;对多线程 Get,串行化是性能上限,但实现极简。

### 8.3 user_data 编码三档演进(D16)

| 阶段 | 编码 | 用途 |
|---|---|---|
| **M3–M6**(Model A,1:1) | `user_data = 0` | 不使用 |
| **M7**(batch API) | `user_data = 数组下标` | `WriteBlocks(span)` 把 batch 中第 i 个 op 的 cqe 路由到 results[i] |
| **未来 Model B**(reactor 阶段评估) | `user_data = (uint64_t)Request*` | reaper 线程通过指针唤醒等待者 |

**M7 batch API 编码示例**:

```
WriteBlocks(span<pair<BlockId, BufferHandle*>> blocks):
  std::lock_guard lk(io_mutex_);
  in_flight_count_ += blocks.size();
  
  for (size_t i = 0; i < blocks.size(); ++i) {
      sqe = io_uring_get_sqe(ring_);
      prep_write_fixed(sqe, ..., blocks[i].handle);
      sqe->user_data = i;            // ← 数组下标,M7 启用
      sqe->flags |= IOSQE_FIXED_FILE;
  }
  
  io_uring_submit_and_wait(ring_, blocks.size());
  
  std::vector<int32_t> results(blocks.size());
  io_uring_for_each_cqe(ring_, head, cqe) {
      size_t i = cqe->user_data;
      results[i] = cqe->res;
      ...
  }
  io_uring_cq_advance(ring_, blocks.size());
  in_flight_count_ -= blocks.size();
  
  // 任何 results[i] < 0 → 返回 IO_BACKEND_IO_FAILED;否则 SUCCESS
```

### 8.4 锁层级文档化

```
Engine.mutex_(shared_mutex)
    │
    ▼ 调 IoBackend 方法(无 reentrant)
    │
IoUringIoBackend.io_mutex_         ← Model A
                pool_mutex_         ← AcquireBuffer / ReturnBuffer_Internal
```

**严格单向**:Engine 锁 → IoBackend 锁。任何 IoBackend 方法不得反向调用 Engine。

---

## 9. 状态机与 Close drain(D9、D17)

### 9.1 状态变量

| 变量 | 类型 | 含义 |
|---|---|---|
| `opened_` | bool | Open 成功后 true,Close 后 false |
| `closed_` | `atomic<bool>` | 一旦 Close 进入终态即 true,永不复位(P3 Q7) |
| `outstanding_count_` | `atomic<uint32_t>` | 已 AcquireBuffer 但未 dtor 的 BufferHandle 数(P3 Q7) |
| `in_flight_count_` | `atomic<uint32_t>` | 已 submit 但 cqe 未 seen 的 op 数(P4 新增) |

注意:`outstanding_count_` 与 `in_flight_count_` 跟踪不同对象。

- `outstanding_count_`:用户侧 BufferHandle 的生命周期
- `in_flight_count_`:io_uring ring 内的 op 生命周期

一个 op 的完整生命周期:
1. 调用方拿 handle(`outstanding_count_++`)
2. 调 WriteBlock/ReadBlock,内部 submit(`in_flight_count_++`)
3. wait_cqe + cqe_seen(`in_flight_count_--`)
4. 调用方析构 handle(`outstanding_count_--`)

### 9.2 状态机

```
                ┌────────────────┐
                │ 未 Open         │
                │ opened_=false   │
                │ closed_=false   │
                └────────────────┘
                        │
                        │ Open() 成功
                        ▼
                ┌────────────────┐
                │ Opened          │
                │ opened_=true    │   ← Put/Get/Delete 在此态执行
                │ closed_=false   │
                └────────────────┘
                        │
                        │ Close() 调用
                        ▼
                ┌────────────────┐
                │ Closed(终态)    │
                │ opened_=false   │
                │ closed_=true    │
                └────────────────┘

任何 Closed 态后的 Open / Read / Write / AcquireBuffer
  → IO_BACKEND_NOT_OPEN
```

### 9.3 Close drain 流程(D17)

```
Close():
  if (!opened_) {
      // P3 Q7:Close 在未 Open 上幂等 SUCCESS
      return SUCCESS;
  }
  
  closed_.store(true, memory_order_release);
  // ↑ 任何在 closed_=true 之后开始执行的方法都拒绝
  //   已经在临界区里执行的方法持锁继续完成,会让 in_flight_count_ 短暂上升
  
  // === drain ===
  std::lock_guard lk(io_mutex_);     // 等当前提交方退出
  while (in_flight_count_.load(memory_order_acquire) > 0) {
      io_uring_cqe* cqe = nullptr;
      ret = io_uring_wait_cqe(ring_, &cqe);
      if (ret == 0) {
          io_uring_cqe_seen(ring_, cqe);
          in_flight_count_.fetch_sub(1, memory_order_release);
      }
      // ret < 0 是异常,但 D17 假定运行环境健壮,不带超时,继续等
  }
  
  // === Q7 outstanding handle 检查 ===
  if (outstanding_count_.load() > 0) {
      #ifdef NDEBUG
        log_warn("Close called with %u outstanding BufferHandle(s); leaking slots",
                 outstanding_count_.load());
      #else
        std::abort();
      #endif
  }
  
  // === 反注册 + queue_exit + munmap + close fd ===
  io_uring_unregister_files(ring_);
  io_uring_unregister_buffers(ring_);
  io_uring_queue_exit(ring_);
  munmap(pool_base_, pool_total_size_);
  close(fd_);
  
  opened_ = false;
  return SUCCESS;
```

### 9.4 drain 不带超时(D17 + 健壮运行环境假设)

参考:`memory/feedback_robust_runtime_assumption.md`。

- Cabe 假定 OS 与硬件正常工作;不为"内核 hang"、"硬盘失联但 fd 仍开"、"DMA 超时不返回"等场景兜底
- `wait_cqe` 不返回 → 是 OS/硬件 bug,应让 hang detector / 运维监控介入,不是库的责任
- 引入超时反而引入新失败模式:超时后 close fd 会让 in-flight DMA 走向未定义,不能安全恢复

---

## 10. 错误处理(D8)

### 10.1 io_uring 错误源 → `IO_BACKEND_*` 映射

| io_uring 错误源 | 检测点 | 映射 |
|---|---|---|
| `io_uring_queue_init` 返回 < 0 | Open | IO_BACKEND_NOT_OPEN(Open 整体失败) |
| `io_uring_register_buffers` 返回 < 0 | Open | 同上(D15) |
| `io_uring_register_files` 返回 < 0 | Open | 同上 |
| `io_uring_submit*` 返回 -EAGAIN | Submit | 退避一次重试;仍失败 → `IO_BACKEND_SUBMIT_FAILED` |
| `io_uring_submit*` 返回其他负值 | Submit | `IO_BACKEND_SUBMIT_FAILED` |
| `cqe->res >= 0` 但 ≠ 期望字节数 | 完成 | `IO_BACKEND_IO_FAILED`(短读/短写视为错误) |
| `cqe->res < 0` | 完成 | `IO_BACKEND_IO_FAILED`(不区分具体 errno) |
| 在 closed_=true 上调用 | 入口 | `IO_BACKEND_NOT_OPEN` |
| `handle.valid() == false` 或跨 backend | 入口 | `IO_BACKEND_INVALID_HANDLE` |
| pool 耗尽 | AcquireBuffer | 返回 invalid handle(Q3),不是错误码 |

### 10.2 不引入新错误码

P4 不为 io_uring 新增错误码。理由:
- `engine_api.cpp::TranslateStatus` 已经给 `IO_BACKEND_*` 7 条 case 写好了
- 现有 7 条桶能完整表达 io_uring 的所有失败语义
- 新增码会让 `TranslateStatus` 又改一轮,上层 `Status` 也可能要新增

---

## 11. Sanitizer 策略(D19)

### 11.1 阻断矩阵

| 组合 | P4 处置 | 实现位置 |
|---|---|---|
| `--backend=sync` + 任意 sanitizer | ✅ 全支持(沿用 P3) | 无需改动 |
| `--backend=io_uring` + 无 sanitizer | ✅ 主路径 | M1 落地 |
| `--backend=io_uring` + ASAN | ✅ 兼容(ASAN 不监控内存序) | M1 落地 |
| `--backend=io_uring` + UBSAN | ✅ 兼容 | M1 落地 |
| `--backend=io_uring` + **TSAN** | ❌ **CMake FATAL_ERROR + scripts 提前 reject** | §11.2 |

### 11.2 阻断点

#### CMake 层(底裤,不可绕过)

`CMakeLists.txt` 在 `CABE_IO_BACKEND` 与 `CABE_ENABLE_TSAN` 都解析完之后:

```cmake
if(CABE_IO_BACKEND STREQUAL "io_uring" AND CABE_ENABLE_TSAN)
    message(FATAL_ERROR
        "CABE_IO_BACKEND=io_uring 与 CABE_ENABLE_TSAN=ON 不兼容。\n"
        "io_uring 的 SQ/CQ 是用户态与内核共享内存,TSAN 看不到内核侧 store,\n"
        "会产生大量 false-positive race 报告。\n"
        "请改用 -DCABE_IO_BACKEND=sync 跑 TSAN,\n"
        "或不开 TSAN 跑 io_uring。")
endif()
```

#### scripts 层(用户体验)

`scripts/run-tests.sh`:

```bash
if [[ "$BACKEND" == "io_uring" && "$SAN_SUFFIX" == "-tsan" ]]; then
    cat <<EOF >&2
ERROR: --backend=io_uring 与 --tsan 不兼容。

io_uring 的 SQ/CQ 是用户态与内核共享内存,TSAN 看不到内核侧 store,
会产生大量 false-positive race 报告。

请改用:
  ./scripts/run-tests.sh --tsan                   (sync + TSAN)
  ./scripts/run-tests.sh --backend=io_uring       (io_uring + 无 sanitizer)
  ./scripts/run-tests.sh --backend=io_uring --asan
EOF
    exit 2
fi
```

`scripts/run-bench.sh`:bench 不开 sanitizer,不需要此 check(已有 ASAN/TSAN/UBSAN=OFF 强制)。

### 11.3 为什么不阻断 ASAN/UBSAN + io_uring

- ASAN 监控的是用户态内存访问越界 / 释放后使用 / 未初始化等,与共享 ring 内存无冲突
- UBSAN 监控未定义行为(整数溢出、空指针解引用、对齐不当等),也与 ring 无冲突
- 这两个组合反而能在开发期抓 io_uring 后端的内存安全 / UB bug,有意义

---

## 12. 公开 / 内部 API 契约

### 12.1 公开 API(不变)

`IoBackendTraits` 七个方法签名(`io/io_backend.h:57`):

```cpp
{ t.Open(path, poolCount) }        -> std::same_as<int32_t>
{ t.Close() }                      -> std::same_as<int32_t>
{ ct.IsOpen() }                    -> std::same_as<bool>
{ ct.BlockCount() }                -> std::same_as<uint64_t>
{ t.AcquireBuffer() }              -> std::same_as<BufferHandle>
{ t.WriteBlock(blockId, chandle) } -> std::same_as<int32_t>
{ t.ReadBlock(blockId, handle) }   -> std::same_as<int32_t>
```

`IoUringIoBackend` 必须满足全部七项,签名一字不变。

`Engine::Put / Get / Delete / Remove / Open / Close / Size / IsOpen` —— 行为一字不变。

### 12.2 内部 API 新增(M7,可选交付)

```cpp
namespace cabe::io {
class IoUringIoBackend {
    // ...P4 主线方法...

    // M7 新增(SyncIoBackend 也要补对应实现作 fallback,即 for-loop 调单 op)
    int32_t WriteBlocks(std::span<const std::pair<BlockId, const BufferHandle*>> blocks);
    int32_t ReadBlocks (std::span<const std::pair<BlockId, BufferHandle*>>      blocks);
};
} // namespace
```

**契约**:
- 中途任一 op 失败不影响其他已发出的 op
- 返回值汇总:全部 SUCCESS → SUCCESS;否则返回首个非 SUCCESS 错误码
- 内部用 `user_data = 数组下标` 关联

**SyncIoBackend 的 fallback 实现**:就是 for-loop 调用单 op 版本。等价语义,无加速。

### 12.3 Options 字段新增

`include/cabe/options.h` 新增:

```cpp
struct Options {
    // 已有字段(不动)
    uint32_t buffer_pool_count = 16;
    // ...
    
    // P4 新增
    uint32_t io_uring_sq_depth = 64;       // ← D7,SQ 深度
};
```

**约束**(Open 时校验):
- `io_uring_sq_depth >= buffer_pool_count`(否则 in-flight 上限会卡死 Acquire 路径)
- `io_uring_sq_depth` 必须是 2 的幂(io_uring 内部约束)
- sync 后端忽略此字段

---

## 13. 里程碑(M1–M9)

### M1 — 骨架 + liburing 接入

**范围**:
- 新增 `io/backends/io_uring_io_backend.{h,cpp}` 类骨架(stub 方法,返回 IO_BACKEND_NOT_OPEN)
- 新增 `io/backends/io_uring_buffer_handle_impl.h`
- `CMakeLists.txt` io_uring 分支取消 FATAL_ERROR,加 `pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.9)`,`target_link_libraries(cabe_lib PUBLIC PkgConfig::LIBURING)`
- `CMakePresets.json` 加 `io_uring-debug` / `io_uring-asan` 两个 preset,移除现有 `io_uring-release` 描述里的 "currently FATAL_ERROR" 文案
- 新增 `test/io/io_uring_skeleton_test.cpp`(类型别名 / concept / 默认构造 / move 语义,无设备依赖)
- D19 阻断检查(CMake + scripts 双层)

**验收**:
- `./scripts/run-tests.sh --backend=io_uring` 编译通过
- `IoBackendTraits<IoBackend>` 对 io_uring 满足(`static_assert` 通过)
- skeleton test 全绿
- `--backend=io_uring --tsan` 在 CMake 与脚本两层都 reject

**失败回退**:M1 失败说明 liburing 接入或 PIMPL 友元配置有问题,不应进 M2。

---

### M2 — Open / Close + buffer pool 真实实现

**范围**:
- `Open(path, count)`:开 fd → mmap pool → queue_init → register_buffers → register_files → opened_=true
- `Close()`:closed_=true → drain(此时 in_flight 必为 0) → Q7 检查 → unregister_files → unregister_buffers → queue_exit → munmap → close fd
- `AcquireBuffer / IsOpen / BlockCount / is_closed`
- BufferHandleImpl 析构调 `owner_->ReturnBuffer_Internal`

**验收**:
- `io_backend_contract_test.cpp` 中"Open / Close / AcquireBuffer / handle 生命周期"用例对 io_uring 全绿
- `--backend=io_uring --asan` / `--ubsan` 跑 contract test 也全绿

**失败回退**:Open 路径出错可能涉及 pkg-config / liburing 头版本不匹配。

---

### M3 — 朴素 WriteBlock / ReadBlock(无 FIXED,无 register_files)

**范围**:
- `WriteBlock` / `ReadBlock` 使用 `io_uring_prep_write` / `io_uring_prep_read`(裸指针、裸 fd)
- Model A:`io_mutex_` 包住 submit + wait
- 不使用 `IOSQE_FIXED_FILE`,不使用 `WRITE_FIXED`(M4 才上)
- 对应错误映射

**验收**:
- `io_backend_contract_test.cpp` 全 18 用例对 io_uring 全绿
- `engine_test.cpp` / `engine_thread_test.cpp` 对 io_uring 全绿
- 跑一次 bench 归档 `p4-pre-fixed`(用于 M4 / M5 对比)

**失败回退**:Model A 实现错误可能造成死锁,bisect 到此里程碑。

---

### M4 — register_buffers + WRITE_FIXED / READ_FIXED

**范围**:
- Open 末尾加 `io_uring_register_buffers(ring, iovecs, n)`,iovecs 是 n × 1 MiB
- BufferHandleImpl 加 `fixed_buf_index_` 字段(io_uring 后端版本)
- WriteBlock / ReadBlock 改用 `prep_write_fixed` / `prep_read_fixed`
- D15:register_buffers 失败 → Open 整体失败

**验收**:
- 所有 M3 测试再过一遍
- bench 对比 `p4-pre-fixed`,出现可观测加速(≥ 5%)
- `RegisterBuffersFailsWhenPoolTooLarge` 新测试通过(强制 ulimit -l 64KiB 模拟 RLIMIT_MEMLOCK 撞限)

**失败回退**:加速不显著(< 5%)说明 GUP 不是当前 bench 场景的瓶颈,需检查 bench 设备 / governor。

---

### M5 — register_files + IOSQE_FIXED_FILE

**范围**:
- Open 加 `io_uring_register_files(ring, &fd_, 1)`
- WriteBlock / ReadBlock 的 SQE 加 `flags |= IOSQE_FIXED_FILE`,`fd` 字段填 0(registered file index)
- Close 加 `io_uring_unregister_files`

**验收**:
- 所有 M4 测试再过一遍
- bench 对比 `p4-post-fixed`(归档),应再加速(数值小,~1%–3%,可接受)
- 归档 `p4-post-fixed`

**失败回退**:本里程碑改动小,出错通常是 IOSQE flag 写错或 fd_idx 错位。

---

### M6 — Close drain 契约 + io_uring 专属测试 + 文档

**范围**:
- `test/io/io_uring_specific_test.cpp` 新增:
    - `CloseDrainsInflightSubmissions`(用 reactor-style 制造长 in-flight,Close 必须等)
    - `RegisterBufferIndexMatchesSlot`(用 FIXED 写入后用 sync backend 验证)
    - `RegisterBuffersFailsWhenPoolTooLarge`(M4 已写,这里 review)
- `README.md` 加 "Production deployment notes" 章节(ulimit / systemd 配置)
- D7 SQ depth sweep:1 / 8 / 64 / 256 跑 bench,选定默认值

**验收**:
- 所有专属测试全绿
- README 文档 review 通过
- SQ depth 默认值有 bench 数据支撑

**失败回退**:专属测试发现 contract 漏洞 → 退到 M3 重新设计。

---

### M7 — 内部 batch API + Engine 多 chunk 路径接入

**范围**:
- 新增 `WriteBlocks` / `ReadBlocks` 在 IoBackend(IoUringIoBackend 真实实现 + SyncIoBackend for-loop fallback)
- `Engine::Put` 多 chunk 路径改用 `WriteBlocks`
- `Engine::Get` 多 chunk 路径改用 `ReadBlocks`
- `engine_bench` / `cabe_engine_bench` 跑大 value Put / Get 对比

**验收**:
- 所有契约测试 + Engine 测试对 io_uring 全绿
- 大 value(16 MiB)Put / Get bench 显著加速(预期 ≥ 30%,实际待数据)
- 归档 `p4-post-batch`

**时机**:见 D18,M3–M6 稳定后再做;不强制紧跟 M6。如果 bench 数据已经达到预期硬件利用率,M7 可推到 P5 启动前再做。但**仍属于 P4 工作项**,不能无限期延期。

**失败回退**:Engine 多 chunk 改动出错 → 退到 M6 状态;batch API 保留只在 IoBackend 内部,Engine 不接入。

---

### M8 — (可选)Model A → Model B 升级评估

**范围**:
- 评估 M7 之后多线程 Get 是否有真正 overlap 收益
- 如果数据显示 Model A 吃满 → Model B 不必做
- 如果数据显示 Model A 是瓶颈 → 设计 reaper 线程,bench 验证

**M8 触发条件**(决策树):

```
M7 完成,跑多线程 Get bench
   │
   ├── Model A 吞吐已达 NVMe 单设备极限(%CPU 不饱和)→ 不做 M8
   │
   └── Model A 吞吐未达极限,且 Engine 锁不是瓶颈 → 做 M8
```

**验收**:多线程 Get 吞吐有可观测提升;reaper 线程不引入新的死锁 / livelock。

**何时不做**:Model A 已经够用 → 直接跳到 M9。

---

### M9 — bench 基线归档 + README / 路线图收尾

**范围**:
- 整理 `bench/baselines/` 下 P4 阶段的所有归档(`p4-pre-fixed` / `p4-post-fixed` / `p4-post-batch`,可能加 `p4-post-modelB`)
- `README.md` Roadmap 表 P4 → ✅ 完成
- `memory/project_roadmap.md` 加 "P4 实现摘要"(类似 P3 实现摘要的格式)
- 可选:加一个 `bench/baselines/p4-summary.md` 总结各档收益

**验收**:
- 文档全部更新
- 路线图状态推进

---

## 14. 风险点(R1–R12)

| ID | 风险 | 触发条件 | 处置 | 归属里程碑 |
|---|---|---|---|---|
| R1 | `RLIMIT_MEMLOCK` 撞限 | pool 较大或系统 64KiB 默认 | D15:Open 失败 + README 文档化 | M2 / M4 / M6 |
| R2 | TSAN false positive | TSAN + io_uring | D19:CMake + scripts 双层阻断 | M1 |
| R3 | O_DIRECT\|O_SYNC 持久化语义偏离 | 写完成 vs 介质落盘的时序 | 健壮运行环境假设(memory) → 不做主动 fsync 校验,信任 O_SYNC + io_uring 语义 | M3 |
| R4 | 内核 worker punt 拖慢延迟 | 部分 op 被踢到 io_uring kernel worker | 不主动加 IOSQE_ASYNC;若 bench 显示稳定的 worker punt 才考虑深挖 | M5 |
| R5 | liburing API 跨版本 breaking | Fedora 包升级到 3.x 假设 | pkg-config 锁 `>= 2.9`;CI 跟 Fedora 滚动 | 长期 |
| R6 | M4 加速不及预期(< 5%) | bench 设备 / governor / pool 大小不当 | 退到 §13 M4 失败回退;检查环境再跑 | M4 |
| R7 | Hardened kernel / Docker seccomp 禁 io_uring | 用户在受限环境跑 | Open 时返回 IO_BACKEND_NOT_OPEN;README 列已知不支持环境 | M2 |
| R8 | 多线程 SQ 竞争 | Model A 之外的方案下 | Model A 用 `io_mutex_` 强制串行,自动满足 | M3 |
| R9 | Close drain 与 BufferHandle dtor 时序 | 用户 close 后 handle 仍在栈上 | drain 完才解锁;handle dtor 看到 closed_=true 走 Q7 force-release | M2 / M6 |
| R10 | `io_uring_queue_exit` 与未消费 CQE | exit 时仍有 CQE 未 seen | drain 完整收齐才 exit;契约测试覆盖 | M2 / M6 |
| R11 | Engine 锁 + IoBackend 锁层级混乱 | 反向调用 | 文档化:Engine.mutex_ → IoBackend.io_mutex_ 单向 | §8.4 |
| R12 | `Options.buffer_pool_count` vs `sq_depth` 关系误解 | 用户配 sq_depth < pool_count | Open 校验失败,返回 IO_BACKEND_NOT_OPEN 或参数错误 | M2 |

---

## 15. 测试策略

### 15.1 共性契约测试(复用 P3)

`test/io/io_backend_contract_test.cpp`(18 用例,需 `CABE_TEST_DEVICE`)对 io_uring backend 必须全绿。验收命令:

```bash
sudo ./scripts/mkloop.sh create
export CABE_TEST_DEVICE=/dev/loopX
./scripts/run-tests.sh --backend=io_uring --filter 'IoBackendContract'
```

### 15.2 io_uring 专属测试(M6 新增)

`test/io/io_uring_specific_test.cpp`:

| 测试 | 目的 |
|---|---|
| `RegisterBuffersFailsWhenPoolTooLarge` | RLIMIT_MEMLOCK 撞限路径(D15 验证) |
| `CloseDrainsInflightSubmissions` | drain 必须等所有 cqe(R9 R10 验证) |
| `RegisterBufferIndexMatchesSlot` | FIXED 写入后用 sync 验证内容(M4 验证) |
| `OpenRejectsSqDepthLessThanPoolCount` | sq_depth < pool_count 时 Open 失败(R12) |
| `WriteBlockEAGAINRetriesOnce` | SQ 满时退避重试(§10.1) |

### 15.3 Engine 端到端

`test/engine/engine_test.cpp` / `engine_thread_test.cpp` / `cabe_engine_thread_test.cpp` 对 io_uring 后端必须全绿。这是确认公开 API 行为零回归。

### 15.4 Sanitizer 矩阵

| 组合 | 跑不跑 |
|---|---|
| `--backend=sync --asan` | 跑(P3 已有) |
| `--backend=sync --tsan` | 跑(P3 已有) |
| `--backend=sync --ubsan` | 跑(P3 已有) |
| `--backend=io_uring`(无 san) | 跑 |
| `--backend=io_uring --asan` | 跑 |
| `--backend=io_uring --ubsan` | 跑 |
| `--backend=io_uring --tsan` | **拒绝**(D19) |

### 15.5 bench 归档计划

| 标签 | 归档时机 | 用途 |
|---|---|---|
| `p3-final` | P3 收尾(可能已有) | sync 后端基线 |
| `p4-pre-fixed` | M3 完成后 | io_uring 朴素路径 |
| `p4-post-fixed` | M4/M5 完成后 | io_uring + FIXED + register_files |
| `p4-post-batch` | M7 完成后 | + batch API |
| `p4-post-modelB` | M8 完成后(若做) | + reaper |

每次归档执行(在 Linux 端):

```bash
./scripts/run-bench.sh --backend=io_uring --archive p4-post-fixed
```

---

## 16. 工程原则关联

P4 的设计在多处体现两条已归档的工程原则,这里总结映射关系。

### 16.1 先落地后优化(`memory/feedback_engineering_principle.md`)

| 体现点 | 描述 |
|---|---|
| Model A 起步 | 不直接上 reaper 线程的 Model B,先用粗 mutex 验证整条 io_uring 路径 |
| M3 朴素 → M4 FIXED → M5 register_files | 逐档独立交付,独立验收,独立 bench 归档 |
| M7 不强制紧跟 M6 | 主线稳定后再上 batch,避免改动叠加 |
| M8 设决策树而非直接做 | M8 是否做取决于 M7 数据,不预先承诺 |
| 不引入 SQPOLL / IOPOLL | 这两个属于"压榨极限"的优化档,推到后续阶段独立评测 |

### 16.2 健壮运行环境假设(`memory/feedback_robust_runtime_assumption.md`)

| 体现点 | 描述 |
|---|---|
| drain 不带超时(D17) | 假定 wait_cqe 终会返回 |
| 不主动 fsync 校验持久化(R3) | 信任 O_SYNC + io_uring 语义,不为内核 bug 兜底 |
| 不监测 worker punt(R4) | 不为 io_uring 内部调度做应用层兜底 |
| RLIMIT_MEMLOCK 失败硬退出(D15) | 不试图 fallback 维持 backwards compat,环境配错就改环境 |
| Q7 outstanding handle 仍是 Debug abort / Release warn | 编程错误 vs 运行环境异常的区分:这是编程错误,继续抓 |

### 16.3 反例:不在 P4 做的事

- 不为"硬盘掉线但 fd 仍开"写代码
- 不为"内核 hang"加超时
- 不引入"健康检查"线程
- 不为"用户传错 BlockId"加越界恢复(直接错误码)

---

## 17. CMake 与脚本变更点(M1 落地清单)

### 17.1 `CMakeLists.txt`

| 改动 | 行号(当前) | 内容 |
|---|---|---|
| 加 pkg-config | `~166`(在 CABE_IO_BACKEND 处理之前) | `find_package(PkgConfig REQUIRED)` |
| io_uring 分支取消 FATAL_ERROR | `172-180` | `target_compile_definitions(cabe_lib PUBLIC CABE_IO_BACKEND_IO_URING=1)` + `pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing>=2.9)` + `target_link_libraries(cabe_lib PUBLIC PkgConfig::LIBURING)` + `target_sources(cabe_lib PRIVATE io/backends/io_uring_io_backend.cpp)` |
| TSAN + io_uring 阻断 | `~192`(在 `message(STATUS "IoBackend...")` 之后) | 见 §11.2 |
| feature 探测(D10) | 新增 try_compile 区段 | `CABE_HAVE_IORING_DEFER_TASKRUN` / `CABE_HAVE_IORING_SINGLE_ISSUER` 等 |
| `CABE_LIB_SOURCES` 加 io_uring 后端文件 | 静态加入(M1)还是 if 分支?推荐:静态加入,保持 sync 后端编译时 io_uring 文件不进 cabe_lib(`target_sources` 在 io_uring 分支里追加) | — |

### 17.2 `CMakePresets.json`

| 改动 | 内容 |
|---|---|
| `io_uring-debug` 新增 | `binaryDir: build-io_uring-debug`、`CABE_BUILD_TYPE=Debug` |
| `io_uring-asan` 新增 | `binaryDir: build-io_uring-asan`、`CABE_ENABLE_ASAN=ON` |
| `io_uring-release` 描述更新 | 移除 "Currently FATAL_ERROR until P4 implementation lands" 文案 |
| `testPresets` 加 io_uring-* 三条 | 沿用 sync 系列的 output / execution 配置 |

### 17.3 `scripts/run-tests.sh`

| 改动 | 内容 |
|---|---|
| TSAN + io_uring 阻断 | 见 §11.2,加在参数解析后、cmake 之前 |

### 17.4 `scripts/run-bench.sh`

| 改动 | 无(bench 强制无 sanitizer,已有保护) | — |

### 17.5 `include/cabe/options.h`

| 改动 | 内容 |
|---|---|
| 新增 `io_uring_sq_depth` 字段 | 默认 64,sync 后端忽略 |
| 文档注释 | 说明 sq_depth >= buffer_pool_count 约束 |

---

## 18. 后续阶段衔接

### 18.1 给 P5 WAL 留什么

- IoBackend 公开 API 不变 → WAL 写就是普通 WriteBlock,无特殊耦合
- buffer pool 已经支持 metadata 类块 → WAL 记录可以直接走 BufferHandle
- 健壮环境假设让 WAL 不需要为"OS 不响应"设计降级路径

### 18.2 给 P6 reactor 留什么

- IoBackend 抽象层不变 → reactor 内部从 Model A 升级到 Model B(D14)是 IoBackend 内部实现细节
- batch API(M7)已经验证了"用 user_data 关联多 op" 的 codebase
- Engine 公开 API 异步化是 P6 的事,P4 不动

### 18.3 给 P7 自研 B+ 树留什么

- B+ 树节点 I/O 复用现有 BufferHandle 路径
- metadata 模型在 P7 期定型(centralized metadata region)
- 节点 buffer 走 io_uring registered → 自动享受 zero-copy

### 18.4 给 P8 scatter-gather 留什么

- batch API(M7)是 scatter-gather 的"中间形态" —— 多个 op 一次 submit
- scatter-gather 是单 op 多 iovec(`prep_writev_fixed`),与 batch 是不同维度
- value 真正的零拷贝在 P8 落地:scatter-gather 把 metadata 和 user value 在同一个 op 内拼接

---

## 19. 跨 LLM 验证 anchor

把以下问题抛给 Gemini / ChatGPT,可筛出谁懂 io_uring:

1. **Model A 性能上限**(§8.2):在 sync 公开 API + Engine shared_lock 下,Model A vs Model B 的多线程 Get 吞吐差距能否给出量化模型?
2. **iovec 注册粒度**(§6.2):n × 1 MiB iovec vs 1 × (n MiB) iovec,在 io_uring 5.13+ 引入 BUFFERS2 之后哪个更优?
3. **持久化语义**(§9.4 / R3):O_DIRECT|O_SYNC fd 上的 IORING_OP_WRITE_FIXED 完成是否等价于 sync pwrite 返回?
4. **drain 设计**(§9.4 / D17):无限等 vs 有界等,哪个更适配嵌入式库定位?
5. **TSAN + io_uring**(§11.3 / D19):除了硬阻断,还有别的方案让两者共存吗?
6. **静态/动态链接性能**(§4.2 / D11):Cabe 的 hot path(单 op)在静态 vs 动态 liburing 下能否给出 ns 级量化?
7. **kernel worker punt**(R4):怎么判断一个 io_uring op 被踢到了 kernel worker?

---

## 20. 待定项 / Open Questions

(落地阶段可能浮出的问题,先记下)

- `io_uring_sq_depth` 默认值是 64 还是 128?M5/M6 跑 sweep 后确定(D7)
- M8 是否做?M7 bench 数据决定(D14、D18)
- `feature gate` 具体 try_compile 哪些宏?D10 给了方向,M1 落地时根据 liburing 2.9 的实际 API 确定具体清单
- README "Production deployment notes" 章节文案细节(M6 落地)

---

## 附录 A:决策表(快速查阅)

```
D1:  liburing                                     ✅
D2:  1 ring / IoBackend 实例                       ✅
D3:  P4 公开 API 保持 sync                         ✅
D4:  保守起步:M3→M4→M5                            ✅
D5:  静态 buffer 注册(Open 一次性)                ✅
D6:  SQPOLL/IOPOLL 不进 P4                         ✅
D7:  SQ depth 默认 64,Options 字段                🔍
D8:  错误码沿用 IO_BACKEND_* 7 种                  ✅
D9:  Close drain in-flight CQE                     ✅
D10: feature gate CABE_HAVE_*                      ✅
D11: 动态链接 liburing                             ✅
D12: liburing >= 2.9                               ✅
D13: n × 1 MiB iovec;value 零拷贝是 P5/P7/P8 议题  ✅
D14: Model A 起步;M7 后评估 B                      ✅
D15: register_buffers 失败 → Open 失败             ✅
D16: user_data 三档演进                            ✅
D17: drain 无限等                                   ✅
D18: M7 在 P4 收尾;时机灵活                        ✅
D19: TSAN + io_uring 双层阻断                      ✅
```

## 附录 B:文件改动清单(M1 落地视图)

```
新增:
  io/backends/io_uring_io_backend.h
  io/backends/io_uring_io_backend.cpp
  io/backends/io_uring_buffer_handle_impl.h
  test/io/io_uring_skeleton_test.cpp           (M1)
  test/io/io_uring_specific_test.cpp           (M6)
  doc/p4_io_uring_design.md                     (本文档)

修改:
  CMakeLists.txt
    - 加 pkg-config + liburing 依赖
    - io_uring 分支取消 FATAL_ERROR
    - TSAN + io_uring 阻断
    - feature gate try_compile
    - CABE_LIB_SOURCES io_uring 文件加入(条件加入)
  
  CMakePresets.json
    - io_uring-debug / io_uring-asan 新增
    - io_uring-release 描述更新
    - testPresets 加 io_uring-* 三条
  
  scripts/run-tests.sh
    - TSAN + io_uring 提前 reject
  
  include/cabe/options.h
    - 新增 io_uring_sq_depth 字段
  
  README.md(M9 收尾)
    - Roadmap 表 P4 → ✅
    - "Production deployment notes" 章节(M6 加)
  
  memory/project_roadmap.md(M9 收尾)
    - P4 实现摘要
```

---

**文档结束**
