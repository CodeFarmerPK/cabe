# Cabe P8-M4 设计：后端写入协议与 `io_uring` 注册缓冲区

> 本里程碑在 P8M3 已完成的 `Put` 路径选择基础上，升级内部 I/O 后端写入协议：
> `Reactor::ExecutePut` 不再只把裸指针传给 `IoBackend::Write`，而是构造后端中立的
> `IoWriteBuffer` 写入缓冲区描述符。`io_uring` 后端基于该描述符对 Cabe 自管 `ValueBuffer`
> 主路径使用注册缓冲区写入；sync 后端继续作为功能正确性基准，忽略注册信息并执行普通写入。
>
> P8M4 以未来 SPDK 为长期方向：`io_uring` 是当前阶段的过渡实现，用于验证后端写入描述符和
> Cabe 自管值缓冲区主路径。本文不引入 SPDK 依赖，不实现 SPDK 后端，不扩大公开 API。
>
> **本文为详细设计**，汇总 P8M4-D1 ~ P8M4-D16 的全部裁决。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P8 / M4 |
| 状态 | ✅ 已实装 |
| 上游依赖 | P1 ~ P7 已完成；P8M1 公开 `ValueBuffer` API 已实装；P8M2 `ValueBufferPool` 已实装；P8M3 `Put` 路径选择已实装 |
| 下游依赖本里程碑 | P8M5 bench、文档与收敛；P10 SPDK 后端 |
| 退出判定 | 见 §13 |

---

## 1. 目标与范围

### 1.1 目标

1. **升级后端写入协议**：把 `IoBackend::Write(block_idx, const std::byte*)` 升级为
   `IoBackend::Write(block_idx, const IoWriteBuffer&)`，让后端能够看到实际写入内存的来源和槽位身份。
2. **保持公开 `Put` 语义不变**：应用端仍然只调用
   `Engine::Put(std::string_view key, DataView value)`，不需要选择写入路径。
3. **让 Cabe `ValueBuffer` 主路径使用注册缓冲区写入**：当 `IoWriteBufferKind::ValueBufferSlot`
   进入 `io_uring` 后端时，使用 `io_uring_prep_write_fixed`。
4. **保持应用端自备内存路径后端兼容**：应用端自备内存和复制回退缓冲区继续走普通写入，不纳入
   `io_uring` 注册缓冲区体系。
5. **保持 sync 后端可用**：sync 后端实现同一写入描述符接口，但只使用 `data / size`，继续通过
   `pwrite` 写入。
6. **保持 WAL / FUA / CRC / 索引语义不变**：P8M4 只改变 value 写入来源表达和 `io_uring` 后端提交方式，
   不改变事务顺序和持久化语义。
7. **为 SPDK 预留后端中立边界**：`IoWriteBuffer` 和 `ValueBufferPool` 只表达 Cabe 自管内存的设备、池、
   槽位和代际，不出现 `io_uring` 或 SPDK 专属字段。
8. **强化严格关闭边界**：`Close()` 完成后，不允许任何旧打开周期资源继续被使用。

### 1.2 交付范围

1. **`io/io_write_buffer.h`**（新建）：
   定义 `IoWriteBufferKind` 和 `IoWriteBuffer`。
2. **`io/io_backend.h`**（修改）：
   `IoBackend` concept 的 `Write` 参数从裸指针升级为 `const IoWriteBuffer&`，`Read` 暂不改。
3. **`io/sync/sync_io_backend.h` / `io/sync/sync_io_backend.cpp`**（修改）：
   sync 后端接受 `IoWriteBuffer`，校验地址和长度后执行普通 `pwrite`。
4. **`io/uring/io_uring_backend.h` / `io/uring/io_uring_backend.cpp`**（修改）：
   新增写入缓冲区注册接口、注册状态、固定缓冲区写入路径和资源注销逻辑。
5. **`engine/value_buffer_pool.h` / `engine/value_buffer_pool.cpp`**（修改）：
   提供后端中立的槽位视图导出接口，不导出 `iovec` 或后端私有指针。
6. **`engine/reactor.cpp`**（修改）：
   `ExecutePut` 根据实际写入源构造 `IoWriteBuffer`，并调用新的 `dc_.io.Write(block, io_buffer)`。
7. **`engine/engine.cpp`**（修改）：
   `Open` 阶段在 `ValueBufferPool` 创建后、reactor 启动前，调用后端注册值缓冲区槽位。
8. **`test/io/*` / `test/engine/*`**（修改 / 新增）：
   覆盖写入描述符、sync 兼容、`io_uring` fixed buffer 写入、Engine 行为回归和严格关闭边界。
9. **`doc/P8/P8M4_backend_write_protocol_design.md`**（本文）与 `doc/P8/README.md`（更新）：
   固定 P8M4 设计边界。

### 1.3 明确不做

| 不做项 | 归属 |
|---|---|
| 修改公开 `Put` 签名 | 不做 |
| 新增 `Put(ValueBuffer&&)` 或“强制零拷贝”接口 | 不做 |
| 新增公开路径统计接口 | 不做 |
| 新增高频路径日志 | 不做 |
| 应用端自备内存注册 / 临时注册 | 不做 |
| 复制回退缓冲区注册 | 不做 |
| `IoBackend::Read` 描述符升级 | 不做 |
| 读路径零拷贝公开 API | 后续独立阶段 |
| SPDK 后端或 SPDK 大页内存真实接入 | P10 |
| 批量提交 / 写入流水线化 | 后续性能阶段 |
| bench 和性能档案归档 | P8M5 |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 |
|---|---|---|
| **P8M4-D1** | 里程碑边界 | 采用 SPDK 导向的方案 B：只升级内部后端写入协议，并让 Cabe `ValueBuffer` 主路径在 `io_uring` 后端使用注册缓冲区写入；`io_uring` 只是验证该抽象的过渡实现。 |
| **P8M4-D2** | 写入缓冲区描述符 | 新增内部 `IoWriteBuffer`，字段为 `data / size / kind / device_id / pool_id / slot_index / slot_generation`；不包含 `backend_private` 或 `io_uring` 专属字段。 |
| **P8M4-D3** | `IoBackend::Write` 接口 | `Write` 统一升级为 `Write(std::uint64_t block_idx, const IoWriteBuffer& buffer)`；`Read` 暂不升级；不保留旧裸指针重载。 |
| **P8M4-D4** | 描述符构造位置 | `IoWriteBuffer` 在 `Reactor::ExecutePut` 中构造，必须描述实际写入源。 |
| **P8M4-D5** | `PutWritePlan` 与描述符关系 | 二者不合并；`PutWritePlan` 只负责路径选择，`IoWriteBuffer` 只负责后端写入协议。 |
| **P8M4-D6** | 槽位信息导出 | `ValueBufferPool` 提供后端中立的槽位视图导出接口；不返回 `iovec`，不返回后端私有指针。 |
| **P8M4-D7** | 注册时机 | 注册缓冲区在 `Engine::Open` 阶段按设备池一次性注册，在 `Close` 阶段由后端注销。 |
| **P8M4-D8** | 注册失败语义 | `value_buffer_pool_blocks > 0` 时，若后端注册 Cabe 值缓冲区槽位失败，则 `Engine::Open` 失败并回滚；不静默降级。 |
| **P8M4-D9** | 固定缓冲区使用范围 | 只有 `IoWriteBufferKind::ValueBufferSlot` 使用固定缓冲区写入；`ExternalMemory` 和 `CopyFallbackBuffer` 继续普通写入。 |
| **P8M4-D10** | sync 后端兼容 | sync 后端接受 `IoWriteBuffer`，但只使用 `data / size`，继续普通 `pwrite`；注册接口为空实现并返回成功。 |
| **P8M4-D11** | 读路径 | 不升级读路径，不引入 `IoReadBuffer`。 |
| **P8M4-D12** | WAL / FUA 语义 | 不改变 WAL、FUA、CRC、索引更新和旧块回收顺序。 |
| **P8M4-D13** | `io_uring` fixed write | `ValueBufferSlot` 且槽位索引、地址、长度均匹配已注册缓冲区时，使用 `io_uring_prep_write_fixed`；其他写入继续 `io_uring_prep_write`。 |
| **P8M4-D14** | 资源释放和 move | `io_uring` 后端 `Close` 顺序为：注销注册缓冲区、注销注册文件、销毁 ring、关闭 fd；move 必须完整转移注册状态。 |
| **P8M4-D15** | 可观测性 | 不新增公开路径统计接口，不新增高频路径日志；通过单测、后端测试、Engine 行为测试和 P8M5 bench 验证。 |
| **P8M4-D16** | SPDK 预留 | 采用“槽位身份后端中立，后端内部映射”；`io_uring` 用 `slot_index` 映射 registered buffer index，未来 SPDK 用同一槽位身份映射大页 / DMA 可用内存资源。 |

---

## 3. 核心模型

### 3.1 P8M3 与 P8M4 的分界

P8M3 已经解决“是否直接写入”的问题：

```text
PutValueSource
  -> BuildPutWritePlan
  -> Direct / CopyFallback
```

P8M4 解决“后端如何理解实际写入源”的问题：

```text
Reactor::ExecutePut
  -> 根据 PutWritePlan 选定实际 write_source
  -> 构造 IoWriteBuffer
  -> IoBackend::Write(block_idx, IoWriteBuffer)
```

二者职责必须分开：

| 对象 | 所属层次 | 回答的问题 |
|---|---|---|
| `PutWritePlan` | engine 写入路径层 | 本次 `Put` 直接写入还是复制回退？ |
| `IoWriteBuffer` | I/O 后端协议层 | 后端实际写哪块内存？这块内存是否带有 Cabe 槽位身份？ |

### 3.2 三类写入缓冲区

P8M4 将实际写入源分成三类：

| `IoWriteBufferKind` | 来源 | 后端行为 |
|---|---|---|
| `ExternalMemory` | 应用端自备内存，并被 P8M3 判定可直接写入 | 普通写入 |
| `ValueBufferSlot` | Cabe `ValueBuffer` 主路径，绑定键匹配且设备归属匹配 | `io_uring` fixed buffer 写入；sync 普通写入 |
| `CopyFallbackBuffer` | reactor 私有 `BufferPool` 复制回退缓冲区 | 普通写入 |

关键约束：

```text
只有 ValueBufferSlot 可以使用固定缓冲区写入。
ExternalMemory 和 CopyFallbackBuffer 均不进入注册缓冲区体系。
```

### 3.3 槽位身份后端中立

P8M4 使用 Cabe 自己的槽位身份作为长期抽象：

```text
device_id + pool_id + slot_index + slot_generation
```

后端自行解释该身份：

```text
io_uring:
  slot_index -> registered buffer index

未来 SPDK:
  slot_index -> 大页内存槽位 / DMA 可用 buffer / 后端私有描述符表项
```

engine / reactor 不携带 `iovec`、SPDK 句柄或 `void* backend_private`。

### 3.4 打开周期资源模型

D7 / D8 / D14 / 严格关闭语义共同形成如下模型：

```text
Engine::Open
  -> 打开数据设备后端
  -> 创建 ValueBufferPool
  -> 导出槽位视图
  -> 后端注册槽位资源
  -> 启动 reactor
  -> Open 成功

Engine::Close
  -> 拒绝新请求
  -> 阻止新 ValueBuffer 分配
  -> 等待所有 ValueBuffer 释放
  -> Stop reactor 并 drain 已提交请求
  -> io.Close 注销注册缓冲区 / 注册文件 / ring / fd
  -> 清空旧打开周期资源
```

`Close` 完成后，不允许旧打开周期继续发起任何资源请求，包括读、写、删除、值缓冲区分配、数据块分配和后端资源使用。

---

## 4. `IoWriteBuffer` 设计

### 4.1 文件位置

新增：

```text
io/io_write_buffer.h
```

原因：

- `IoWriteBuffer` 是 I/O 后端协议对象，不属于公开 `Engine` API；
- `reactor` 需要包含它来构造描述符；
- sync 和 `io_uring` 后端都需要包含它；
- 避免 `io` 层依赖 `engine/put_path.h` 或 `ValueBufferPool`。

### 4.2 建议定义

```cpp
#ifndef CABE_IO_WRITE_BUFFER_H
#define CABE_IO_WRITE_BUFFER_H

#include "common/structs.h"

#include <cstddef>
#include <cstdint>

namespace cabe {

    enum class IoWriteBufferKind : std::uint8_t {
        ExternalMemory = 0,
        ValueBufferSlot = 1,
        CopyFallbackBuffer = 2,
    };

    struct IoWriteBuffer {
        const std::byte* data = nullptr;
        std::size_t size = 0;

        IoWriteBufferKind kind = IoWriteBufferKind::ExternalMemory;

        DeviceId device_id = 0;
        std::uint64_t pool_id = 0;
        std::uint32_t slot_index = 0;
        std::uint32_t slot_generation = 0;
    };

} // namespace cabe

#endif // CABE_IO_WRITE_BUFFER_H
```

### 4.3 字段语义

| 字段 | 含义 |
|---|---|
| `data` | 实际写入源地址。 |
| `size` | 实际写入长度；P8M4 必须为 `kValueSize`。 |
| `kind` | 写入源类别。 |
| `device_id` | 写入源所属设备。仅 `ValueBufferSlot` 有强语义。 |
| `pool_id` | 写入源所属 `ValueBufferPool` 身份。 |
| `slot_index` | 池内槽位编号；`io_uring` 后端中直接映射为 registered buffer index。 |
| `slot_generation` | 槽位代际，用于防止过期描述符被误认为当前槽位。 |

### 4.4 明确不包含的字段

| 字段 | 不加入原因 |
|---|---|
| `io_uring_buffer_index` | 绑定过渡后端，不适合 SPDK 方向。 |
| `iovec*` / `iovec_index` | `ValueBufferPool` 不应暴露 `io_uring` 类型。 |
| `spdk_*` | P8M4 不引入 SPDK 依赖。 |
| `backend_private` | 会让 engine / reactor 携带后端私有指针，破坏边界。 |
| `force_zero_copy` | 公开语义不允许“强制零拷贝”；路径由 Cabe 内部判断。 |

---

## 5. `IoBackend` 协议升级

### 5.1 新 concept

`io/io_backend.h` 修改为：

```cpp
#include "io/io_write_buffer.h"

namespace cabe {

    template<typename T>
    concept IoBackend = requires(T& io, const std::string& path,
                                 std::uint64_t block_idx,
                                 const IoWriteBuffer& wbuf,
                                 std::byte* rbuf) {
        { io.Open(path) } -> std::same_as<int32_t>;
        { io.Close() } -> std::same_as<int32_t>;
        { io.BlockCount() } -> std::convertible_to<std::uint64_t>;
        { io.Write(block_idx, wbuf) } -> std::same_as<int32_t>;
        { io.Read(block_idx, rbuf) } -> std::same_as<int32_t>;
    };

} // namespace cabe
```

`Read` 保持裸 `std::byte*`，不引入 `IoReadBuffer`。

### 5.2 不保留旧 `Write` 重载

P8M4 不保留：

```cpp
int32_t Write(std::uint64_t block_idx, const std::byte* buf);
```

原因：

- 避免新旧写入协议长期并存；
- 调用点集中，迁移范围可控；
- 后端测试可以通过构造 `IoWriteBuffer` 验证普通写入；
- P8M4 的目标就是完成写入协议升级。

### 5.3 后端注册接口

需要给后端增加注册值缓冲区槽位的内部接口。建议形态：

```cpp
int32_t RegisterWriteBuffers(std::span<const ValueBufferSlotView> buffers);
```

其中 `ValueBufferSlotView` 的定义和归属见 §6。

sync 后端实现为空操作并返回成功；`io_uring` 后端将槽位视图转换为 `iovec` 后调用
`io_uring_register_buffers`。

---

## 6. `ValueBufferPool` 槽位视图导出

### 6.1 后端中立视图

`ValueBufferPool` 提供槽位视图导出能力：

```cpp
struct ValueBufferSlotView {
    const std::byte* data = nullptr;
    std::size_t size = 0;
    std::uint32_t slot_index = 0;
};
```

推荐接口：

```cpp
std::size_t ExportSlotViews(std::span<ValueBufferSlotView> out) const noexcept;
```

语义：

- 返回实际写入到 `out` 的槽位数量；
- `out.size()` 小于 `slot_count()` 时，只导出前 `out.size()` 个槽位；
- `slot_count() == 0` 时返回 0；
- 导出顺序必须与槽位编号一致；
- 每个视图的 `size` 必须等于 `kValueSize`。

### 6.2 索引规则

P8M4 冻结：

```text
slot_index == registered buffer index
```

对 `io_uring`：

```cpp
iov[slot_index].iov_base = const_cast<std::byte*>(slot_view.data);
iov[slot_index].iov_len = slot_view.size;
```

写入时：

```cpp
io_uring_prep_write_fixed(sqe, 0, buffer.data, kValueSize, offset, buffer.slot_index);
```

### 6.3 为什么不导出 `iovec`

`ValueBufferPool` 是 engine 内部值缓冲区池，不应知道 `io_uring`。如果导出 `iovec`，未来 SPDK 接入时会产生两套不一致的导出接口。

正确边界是：

```text
ValueBufferPool:
  导出 Cabe 槽位视图

后端:
  把槽位视图转换为自己的注册资源
```

---

## 7. `Engine::Open` 注册流程

### 7.1 打开阶段顺序

P8M4 在当前 `Engine::Open` 阶段一中加入注册步骤：

```text
1. Create / Recover 超级块
2. dc.io.Open(data_path, &options_)
3. 创建 reactor 私有 BufferPool
4. ValueBufferPool::Create(device_id, pool_id, value_buffer_pool_blocks)
5. 导出 ValueBufferPool 槽位视图
6. dc.io.RegisterWriteBuffers(slot_views)
7. dc.value_buffer_pool = value_pool
8. 初始化 block_allocator
9. 打开 WAL / snapshot 或执行恢复
10. 全部设备成功后启动 reactor
```

注册必须发生在 reactor 启动前，避免运行期出现“Open 已成功但主路径资源还未准备好”的状态。

### 7.2 注册失败回滚

当 `value_buffer_pool_blocks > 0` 且注册失败：

```text
Engine::Open 返回错误
已打开的数据 / WAL / snapshot 设备回滚关闭
已创建的 ValueBufferPool 被释放
已启动的 reactor 不应存在
```

不允许静默降级为普通写入。

### 7.3 容量为 0

当 `value_buffer_pool_blocks == 0`：

```text
ValueBufferPool 创建空池
ExportSlotViews 返回 0
RegisterWriteBuffers 接收空 span 并成功返回
Engine::Open 成功
AllocateValueBuffer 返回 kEnginePoolExhausted
普通 Put 不受影响
```

容量 0 是显式关闭公开值缓冲区能力，不是失败降级。

---

## 8. `Reactor::ExecutePut` 集成

### 8.1 描述符必须描述实际写入源

P8M4 中 `IoWriteBuffer` 由 `Reactor::ExecutePut` 构造。它不能只描述原始 `op->value`，必须描述最终传给后端的实际写入源。

### 8.2 推荐流程

```text
ExecutePut
  -> Acquire block
  -> BuildPutWritePlan(op->value, op->value_source, caps)
  -> 默认 write_source = op->value.data()
  -> 默认 io_kind 按 op->value_source 判断
  -> 如果 plan.path == CopyFallback:
       从 BufferPool 申请 fallback buffer
       memcpy(op->value -> fallback)
       write_source = fallback
       io_kind = CopyFallbackBuffer
  -> 计算 CRC(write_source)
  -> 构造 IoWriteBuffer
  -> dc_.io.Write(block_idx, io_write_buffer)
  -> 释放 fallback buffer
  -> 写 WAL / 更新索引 / 回收旧块 / 快照触发
```

### 8.3 描述符构造规则

| 情况 | `IoWriteBufferKind` | 描述符字段 |
|---|---|---|
| `plan.path == Direct` 且 `source.kind == TargetKeyValueBuffer` | `ValueBufferSlot` | 使用 `source.device_id / pool_id / slot_index / slot_generation` |
| `plan.path == Direct` 且 `source.kind == ExternalValueMemory` | `ExternalMemory` | 仅 `data / size / kind` 有强语义 |
| `plan.path == CopyFallback` | `CopyFallbackBuffer` | `data` 指向 fallback buffer；槽位字段清零 |

复制回退后，不允许继续把原始 `ValueBufferSlot` 身份传给后端。

### 8.4 CRC 与生命周期

CRC 仍基于实际写入源计算：

```text
CRC(DataView{io_write_buffer.data, kValueSize})
```

P8M4 不改变 P8M3 的生命周期约束：`Put` 调用期间，应用端必须保证传入 value 存活且内容不可变。

---

## 9. sync 后端设计

### 9.1 写入行为

sync 后端接受 `IoWriteBuffer`，但只使用 `data` 和 `size`：

```cpp
int32_t SyncIoBackend::Write(std::uint64_t block_idx, const IoWriteBuffer& buffer) {
    if (block_idx >= block_count_) return err::kIoBase;
    if (buffer.data == nullptr) return err::kIoBase;
    if (buffer.size != kValueSize) return err::kIoBase;

    const std::uint64_t offset = kDataRegionOffset + block_idx * kValueSize;
    if (!io_util::WriteExact(fd_, buffer.data, kValueSize, offset)) {
        return err::kIoBase;
    }

    const WalLevel lvl = opts_ ? opts_->wal_level : WalLevel::WalSync;
    if (IsValueFuaLevel(lvl)) {
        if (::fdatasync(fd_) < 0) return err::kIoBase;
    }
    return err::kSuccess;
}
```

sync 后端忽略：

```text
kind
device_id
pool_id
slot_index
slot_generation
```

### 9.2 注册接口

sync 后端的注册接口为空实现：

```cpp
int32_t RegisterWriteBuffers(std::span<const ValueBufferSlotView>) {
    return err::kSuccess;
}
```

sync 后端是功能正确性基准，不参与固定缓冲区优化。

---

## 10. `io_uring` 后端设计

### 10.1 新增状态

`IoUringIoBackend` 新增：

```cpp
struct RegisteredBufferRecord {
    const std::byte* data = nullptr;
    std::size_t size = 0;
};

bool buffers_registered_ = false;
std::uint32_t registered_buffer_count_ = 0;
std::vector<RegisteredBufferRecord> registered_buffers_;
```

`registered_buffers_` 只用于后端内部校验：

```text
buffer.slot_index < registered_buffer_count_
buffer.data == registered_buffers_[slot_index].data
buffer.size == registered_buffers_[slot_index].size
```

### 10.2 注册流程

```cpp
int32_t IoUringIoBackend::RegisterWriteBuffers(
    std::span<const ValueBufferSlotView> buffers) {
    if (buffers.empty()) return err::kSuccess;
    if (!ring_initialized_ || buffers_registered_) return err::kIoBase;

    std::vector<iovec> iovecs(buffers.size());
    registered_buffers_.resize(buffers.size());

    for (const auto& view : buffers) {
        if (view.data == nullptr || view.size != kValueSize) return err::kIoBase;
        if (view.slot_index >= buffers.size()) return err::kIoBase;

        iovecs[view.slot_index].iov_base = const_cast<std::byte*>(view.data);
        iovecs[view.slot_index].iov_len = view.size;
        registered_buffers_[view.slot_index] = {view.data, view.size};
    }

    const int ret = io_uring_register_buffers(&ring_, iovecs.data(), iovecs.size());
    if (ret < 0) {
        registered_buffers_.clear();
        return err::kIoBase;
    }

    buffers_registered_ = true;
    registered_buffer_count_ = static_cast<std::uint32_t>(iovecs.size());
    return err::kSuccess;
}
```

实现时需要处理 `std::bad_alloc`：后端内部为注册表或 `iovec` 分配内存失败时返回 `err::kEnginePoolExhausted`；
`io_uring_register_buffers` 本身失败时返回 `err::kIoBase`。无论具体错误码是哪一种，注册失败最终都必须使
`Engine::Open` 失败并回滚。

### 10.3 写入分支

```text
if buffer.kind == ValueBufferSlot
   and buffers_registered_
   and slot_index / data / size 校验通过:
       io_uring_prep_write_fixed
else:
       io_uring_prep_write
```

具体规则：

```cpp
if (UseFixedBuffer(buffer)) {
    io_uring_prep_write_fixed(
        sqe, 0, buffer.data, kValueSize, offset,
        static_cast<int>(buffer.slot_index));
} else {
    io_uring_prep_write(sqe, 0, buffer.data, kValueSize, offset);
}
sqe->flags |= IOSQE_FIXED_FILE;
```

这里 `0` 是注册文件描述符索引；`slot_index` 是注册缓冲区索引。

### 10.4 不使用 `IOSQE_BUFFER_SELECT`

P8M4 不使用 `IOSQE_BUFFER_SELECT`。该机制适合由内核从缓冲区组里选择缓冲区，不适合当前写路径。Cabe 当前需要明确指定写入槽位，因此使用 `io_uring_prep_write_fixed`。

### 10.5 完成检查

保持现有同步提交 / 等待模型：

```text
io_uring_submit
io_uring_wait_cqe
cqe->res == kValueSize
user_data == block_idx
```

失败统一返回 `err::kIoBase`，不改变公开错误语义。

### 10.6 FUA 保持不变

写入 CQE 成功后，仍按 `WalLevel` 决定是否 `fdatasync(fd_)`。P8M4 不改为 `RWF_DSYNC` 或 `io_uring` fsync 操作。

---

## 11. 注册资源释放与 move 语义

### 11.1 `Close` 顺序

`IoUringIoBackend::Close()` 顺序：

```text
1. 如果 buffers_registered_:
     io_uring_unregister_buffers(&ring_)
     buffers_registered_ = false
     registered_buffer_count_ = 0
     registered_buffers_.clear()

2. 如果 files_registered_:
     io_uring_unregister_files(&ring_)
     files_registered_ = false

3. 如果 ring_initialized_:
     io_uring_queue_exit(&ring_)
     ring_initialized_ = false

4. 如果 fd_ >= 0:
     close(fd_)
     fd_ = -1
     block_count_ = 0
```

注册缓冲区和注册文件都属于 ring 资源，必须在 `io_uring_queue_exit` 前注销。

### 11.2 析构

析构保持现有兜底语义：

```text
如果 fd_ >= 0，则自动 Close()
```

### 11.3 move 构造

move 构造必须转移：

```text
fd_
block_count_
ring_
ring_initialized_
files_registered_
buffers_registered_
registered_buffer_count_
registered_buffers_
opts_
```

源对象清空：

```text
fd_ = -1
block_count_ = 0
ring_initialized_ = false
files_registered_ = false
buffers_registered_ = false
registered_buffer_count_ = 0
registered_buffers_.clear()
opts_ = nullptr
```

### 11.4 move 赋值

move 赋值前，目标对象若已经打开，必须先释放已有资源；然后按 move 构造同样转移状态。

---

## 12. 严格关闭边界

P8M4 必须延续并强化 P8M2 的严格 `Close` 语义。

### 12.1 基本原则

```text
Close 开始后:
  - 拒绝新公开操作
  - 拒绝新 ValueBuffer 分配
  - 等待已分配 ValueBuffer 释放
  - reactor drain 已提交 op
  - 后端注销注册资源

Close 完成后:
  - 不存在旧打开周期 ValueBuffer
  - 不存在旧打开周期后端注册资源
  - 不允许任何旧打开周期资源请求继续执行
```

### 12.2 覆盖的资源请求

严格关闭边界覆盖：

| 请求 / 资源 | Close 后行为 |
|---|---|
| `Put` | 返回 `kEngineNotOpen` 或被 reactor close-drain 兜底失败 |
| `Get` | 同上 |
| `Delete` | 同上 |
| `AllocateValueBuffer` | 返回 `kEngineNotOpen` |
| 数据块分配 | 不允许在旧 reactor / 旧设备上下文继续发生 |
| 后端注册缓冲区访问 | `io.Close()` 后资源已注销，不允许旧 op 触达 |
| 后端 fd / ring | `io.Close()` 后不可使用 |

### 12.3 P8M4 需要注意的新增点

注册缓冲区生命周期绑定 `IoUringIoBackend` 的 ring。`Close` 必须在 ring 销毁前注销，且 Engine 必须先等待 `ValueBuffer` 释放再停止 reactor，避免后端注销时仍有合法调用方持有旧槽位。

---

## 13. 测试计划与退出条件

### 13.1 单元测试

| 测试 | 目标 |
|---|---|
| `IoWriteBuffer` 类型测试 | 默认值、聚合初始化、字段语义。 |
| `PutPath` 现有测试 | 确认 P8M3 路径选择不变。 |
| `ValueBufferPool` 槽位视图测试 | 导出数量、顺序、地址、长度、`slot_index`。 |
| sync 后端描述符写入测试 | `ExternalMemory` / `ValueBufferSlot` / `CopyFallbackBuffer` 都能普通写入。 |
| `io_uring` 后端注册测试 | 注册 0 个槽位、注册 N 个槽位、重复注册失败或按设计拒绝。 |
| `io_uring` fixed write 测试 | 构造 `ValueBufferSlot` 描述符，通过 `io_uring_prep_write_fixed` 写入并读回。 |
| `io_uring` 普通写入回归 | `ExternalMemory` 和 `CopyFallbackBuffer` 继续普通写入。 |
| move / Close 测试 | 注册状态 move 后不重复注销、不泄漏；Close 后状态清空。 |

### 13.2 Engine 行为测试

| 测试 | 目标 |
|---|---|
| `ValueBufferSameKeyRoundTrip` | Cabe `ValueBuffer` 主路径写入后读回正确。 |
| `ValueBufferDifferentKeyFallbackRoundTrip` | key 不匹配仍复制回退并写入正确。 |
| `OtherDeviceValueBufferFallbackRoundTrip` | 跨设备仍复制回退并写入正确。 |
| `AlignedExternalValueRoundTrip` | 应用自备对齐内存仍普通直接写入并读回正确。 |
| `ConcurrentMixedPutSources` | 多线程混合来源不破坏写入语义。 |
| `CloseWaitsForOutstandingValueBuffer` | Close 等待已分配 `ValueBuffer` 释放。 |
| Close 后资源请求失败 | Close 完成后 `Put / Get / Delete / AllocateValueBuffer` 均不进入旧资源路径。 |

### 13.3 后端矩阵

| 构建 | 要求 |
|---|---|
| sync Debug | 全量测试通过 |
| sync ASAN / UBSAN / TSAN | P8M4 相关测试通过 |
| `io_uring` Release | 后端 fixed write 测试和 Engine 行为测试通过 |
| `io_uring` ASAN / UBSAN | P8M4 相关测试通过 |

继续遵守既有规则：`io_uring` 不跑 TSAN。

### 13.4 退出条件

P8M4 完成时必须满足：

1. `IoWriteBuffer` 定义完成，且不包含后端专属字段。
2. `IoBackend::Write` 已统一升级为描述符接口。
3. `Read` 未被 P8M4 修改。
4. sync 后端通过新接口继续普通写入。
5. `ValueBufferPool` 可导出后端中立槽位视图。
6. `Engine::Open` 在 reactor 启动前完成值缓冲区槽位注册。
7. `io_uring` 后端可注册 Cabe `ValueBufferPool` 槽位。
8. `ValueBufferSlot` 写入使用 `io_uring_prep_write_fixed`。
9. `ExternalMemory` 和 `CopyFallbackBuffer` 继续普通写入。
10. 注册失败时 `Open` 失败并回滚。
11. WAL / FUA / CRC / 索引更新 / 旧块回收语义不退化。
12. `Close` 严格阻断旧打开周期资源继续使用。
13. 不新增公开路径统计接口。
14. 不引入 SPDK 依赖。
15. P8M4 相关测试通过，且 P8M3 既有行为测试不退化。

---

## 14. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 注册缓冲区 pin 住较多内存 | 多设备 / 大容量配置下资源压力增大 | 默认容量保持保守；注册失败 `Open` 失败；容量 0 可关闭能力。 |
| `slot_index` 与注册索引错配 | fixed write 可能写错缓冲区或失败 | 固定 `slot_index == registered buffer index`；后端校验地址和长度。 |
| 描述符携带过多后端细节 | 未来 SPDK 接入受阻 | 不加入 `iovec`、SPDK 字段或 `backend_private`。 |
| sync 与 `io_uring` 行为不一致 | 测试和用户语义分叉 | sync 接受同一描述符并普通写入；公开语义保持一致。 |
| 注册失败静默降级 | 用户以为主零拷贝路径可用但实际不可用 | 注册失败直接 `Open` 失败。 |
| Close 后旧资源继续被访问 | 后端 ring / fd / 注册缓冲区 use-after-close | Close 先阻止新分配并等待 `ValueBuffer` 释放，再 Stop reactor，最后由后端注销资源。 |
| 复制回退路径误带 `ValueBufferSlot` 身份 | 后端错误使用 fixed buffer | `IoWriteBuffer` 在 `ExecutePut` 基于实际写入源构造；回退路径一律 `CopyFallbackBuffer`。 |
| 过早设计读路径 | 扩大 M4 范围、引入公开语义争议 | P8M4 不升级 `Read`，读路径零拷贝留给未来独立阶段。 |
| 高频日志影响性能和硬盘寿命 | 写路径刷盘或日志膨胀 | 不新增路径命中日志，不新增公开统计；用测试和 bench 验证。 |

---

## 15. 对下游的接口承诺

| 下游 | 承诺 |
|---|---|
| P8M5 | bench 可区分非对齐自备内存、对齐自备内存、Cabe `ValueBuffer` 三类 Put；`io_uring` Release 口径可观察主路径收益。 |
| P10 SPDK | 可复用 `IoWriteBuffer` 的槽位身份；SPDK 后端内部建立 `slot_index -> 大页 / DMA 资源` 映射；无需改公开 `Put` API。 |
| 未来读路径零拷贝 | P8M4 不提前定义 `IoReadBuffer`；读路径可独立设计公开语义和生命周期。 |

---

**全文完。**
