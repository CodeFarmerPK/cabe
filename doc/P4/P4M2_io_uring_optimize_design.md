# Cabe P4-M2 设计：预注册文件描述符优化

> 本里程碑在 P4M1 基础上启用 io_uring 的预注册文件描述符（registered files）功能，
> 减少每次 I/O 提交时内核的文件描述符查找和引用计数开销。改动集中在
> `IoUringIoBackend` 的 Open / Close / Write / Read 内部，不改变 IoBackend 接口。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P4 / M2 |
| 状态 | **设计稿** |
| 上游依赖 | P4M1（IoUringIoBackend 基础实现） |
| 下游依赖本里程碑 | P4M3（TSAN 兼容 + 部署文档 + 性能基准） |
| 退出判定 | 见 §7 |

---

## 1. 目标与范围

### 1.1 目标

1. 在 `IoUringIoBackend::Open` 中注册文件描述符（`io_uring_register_files`），减少每次 I/O 的内核开销。
2. Write / Read 提交时启用固定文件标志（`IOSQE_FIXED_FILE`），引用注册索引而非原始 fd。
3. `Close` 时注销文件描述符，保持资源生命周期完整。

### 1.2 交付范围

1. **`io/uring/io_uring_backend.cpp`**（修改）：Open 加注册、Close 加注销、Write / Read 加固定文件标志。
2. **`io/uring/io_uring_backend.h`**（修改）：新增成员 `files_registered_` 标志。
3. 测试不变——现有 10 个用例覆盖正常路径，改动对外行为透明。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 预注册缓冲区（registered buffers） | **P8** | 当前 IoBackend 接口只传内存地址不传索引号；P8 重新设计缓冲区管理时统一处理 |
| 批量提交 | **P7** | P4 为单线程同步模型，每次只有一个请求在飞，批量提交无收益 |

---

## 2. 背景：预注册文件描述符的原理

普通 I/O 流程中，每次提交读写请求，内核需要：
1. 通过 fd 查找内核内部的文件对象
2. 增加引用计数（防止 I/O 进行中文件被关闭）
3. I/O 完成后减少引用计数

预注册文件描述符（`io_uring_register_files`）的做法：在设备打开时一次性告诉内核"这个 fd 我会反复使用"，内核缓存查找结果。后续 I/O 直接引用注册索引号（本场景固定为 0），省去重复查找和引用计数操作。

对 cabe 的场景——每个 `IoUringIoBackend` 绑定一个设备、反复对同一个 fd 做大量 I/O——这个优化路径完全契合。

---

## 3. 决策汇总

| 编号 | 决策 | 结果 | 理由 |
|---|---|---|---|
| **P4M2-D1** | 优化范围 | 只做预注册文件描述符；预注册缓冲区推到 P8 | IoBackend 接口不传缓冲区索引，强行做需侵入 BufferPool 或增加拷贝；P8 重新设计缓冲区管理时统一处理 |
| **P4M2-D2** | 注册时机 | Open 内 ring 初始化后立即注册，Close 时先注销再销毁 ring | 与 ring 生命周期绑定，不单独管理 |
| **P4M2-D3** | 注册失败处理 | 统一返回 `err::kIoBase`，Open 整体失败 | 初期假定系统正常运行，不做降级回退 |

---

## 4. 代码改造

### 4.1 头文件变更

```diff
  // io/uring/io_uring_backend.h
  private:
      static constexpr unsigned kQueueDepth = 64;

      int fd_ = -1;
      std::uint64_t block_count_ = 0;
      struct io_uring ring_{};
      bool ring_initialized_ = false;
+     bool files_registered_ = false;
```

新增 `files_registered_` 标志——控制 Close 时是否需要注销，以及 move 语义时的状态转移。

### 4.2 Open 改造

在 `io_uring_queue_init` 成功之后，追加文件描述符注册：

```cpp
// ring 初始化成功后，注册 fd
int32_t fds[] = {fd_};
ret = io_uring_register_files(&ring_, fds, 1);
if (ret < 0) {
    CABE_LOG_ERROR("io_uring_register_files 失败: ret=%d", ret);
    io_uring_queue_exit(&ring_);
    ::close(fd_);
    fd_ = -1;
    return err::kIoBase;
}
files_registered_ = true;
```

### 4.3 Close 改造

在销毁 ring 之前，先注销文件描述符：

```cpp
if (files_registered_) {
    io_uring_unregister_files(&ring_);
    files_registered_ = false;
}
```

### 4.4 Write / Read 改造

两处改动相同——`io_uring_prep_write` / `io_uring_prep_read` 之后，设置固定文件标志并将 fd 字段改为注册索引：

```diff
  io_uring_prep_write(sqe, fd_, buf, kValueSize, offset);
+ sqe->flags |= IOSQE_FIXED_FILE;
+ sqe->fd = 0;  // 注册索引（只注册了一个 fd，索引固定为 0）
```

Read 同理。

### 4.5 Move 语义

`files_registered_` 与 `ring_initialized_` 同等处理——move 时转移标志，旧对象置 false。

```diff
  // move 构造
  ring_initialized_(other.ring_initialized_)
+ , files_registered_(other.files_registered_)
  {
      ...
+     other.files_registered_ = false;
  }
```

move 赋值同理。

---

## 5. 测试策略

现有 10 个用例（P4M1）不需要修改——预注册文件描述符是纯内部优化，对外行为完全透明（Write / Read 的入参和返回值不变）。

验证方式：
- `run-tests.sh --backend=io_uring --release` 全绿
- `CABE_TEST_DEVICE=/dev/loop0 run-tests.sh --backend=io_uring --asan` 全绿
- Engine 端到端测试（`test_engine`）在 io_uring 后端下全绿

---

## 6. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 注册失败导致 Open 整体失败 | 某些受限环境可能不允许注册 | 初期假定系统正常；未来如需降级可加回退逻辑 |
| 预注册缓冲区推迟到 P8 | P4 无法获得预注册缓冲区的性能收益 | P4 的主要收益来自 io_uring 本身（异步提交/完成）+ 预注册文件描述符；预注册缓冲区在 P8 零拷贝阶段统一处理更干净 |

---

## 7. 退出条件

1. **代码改造完成**：Open 注册 / Close 注销 / Write 和 Read 使用固定文件标志。
2. **现有测试不退步**：io_uring 后端 10 个用例全绿 + Engine 端到端全绿。
3. **sync 后端不退步**：原有全部测试仍全绿。

---

## 8. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P4M3** | 在预注册文件描述符基础上做 TSAN 兼容验证 + 性能基准对比 |
| **P8** | 预注册缓冲区在 P8 实现——需要重新设计缓冲区管理 + 扩展 IoBackend 接口或引入 BufferHandle |

---

**全文完。**
