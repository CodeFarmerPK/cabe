# Cabe P8-M3 设计：`Put` 路径选择与复制回退

> 本里程碑把 P8M2 已实装的 Cabe 值缓冲区池和来源识别能力接入统一 `Put` 写入路径。
> `Engine::Put(key, value)` 仍是唯一公开写入入口；`Engine` 在调用线程内完成目标设备路由和全局值内存
> 来源预识别，`Reactor::ExecutePut` 根据内部写入计划决定直接使用 value 作为写入来源，或复制到
> reactor 私有 `BufferPool` 后写入。
>
> P8M3 的直接写入只表示**跳过 Cabe 内部 1 MiB `memcpy`**。它复用现有
> `IoBackend::Write(block_idx, const std::byte*)`，不升级后端写入协议，不实现 `io_uring` 注册缓冲区，
> 不引入 SPDK。
>
> **本文为详细设计**，汇总 P8M3-D1 ~ P8M3-D14 的全部裁决。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P8 / M3 |
| 状态 | ✅ 已实装 |
| 上游依赖 | P1 ~ P7 已完成；P8M1 公开 `ValueBuffer` API 已实装；P8M2 `ValueBufferPool` 已实装 |
| 下游依赖本里程碑 | P8M4 后端写入协议与 `io_uring` 注册缓冲区；P8M5 bench 与收敛；P10 SPDK 后端 |
| 退出判定 | 见 §14 |

---

## 1. 目标与范围

### 1.1 目标

1. **统一 `Put` 接入路径选择**：保持 `Engine::Put(std::string_view key, DataView value)` 公开签名不变，
   在内部识别 value 来源并选择直接写入或复制回退。
2. **主直接写入路径**：当 value 来自 Cabe 值缓冲区、绑定键与 `Put` 键一致、槽位有效且设备归属匹配时，
   直接把该 value 内存交给 `IoBackend::Write`。
3. **条件式直接写入路径**：当 value 来自应用端自备内存，并满足当前后端直接写入能力要求时，直接交给
   `IoBackend::Write`。
4. **复制回退路径透明**：不能直接写入不是错误；不满足条件时复制到 reactor 私有 `BufferPool` 后写入。
5. **保持写入语义不退化**：WAL、CRC、索引更新、旧 block 回收、错误传播和快照触发顺序保持与 P7/P8M2 一致。
6. **为 P8M4 留出接口边界**：P8M3 不提前引入写入描述符；P8M4 再升级后端协议并接入注册缓冲区。
7. **为 SPDK 留出能力判断边界**：应用端自备内存能否直接写入由后端能力判断决定；普通 `aligned_alloc`
   内存即使 1 MiB 对齐，也不等价于未来 SPDK DMA 可用内存。

### 1.2 交付范围

1. **`engine/reactor.h` / `engine/reactor.cpp`**（修改）：
   `OpNode` 携带值内存来源预识别结果；`ExecutePut` 引入写入计划，按直接写入或复制回退执行。
2. **`engine/engine.cpp`**（修改）：
   `Engine::Put` 在调用线程内完成目标设备路由、扫描所有值缓冲区池、构造 `PutValueSource` 后投递目标
   `reactor`。
3. **`engine/value_buffer_pool.h` / `engine/value_buffer_pool.cpp`**（修改）：
   分配槽位时记录绑定键；来源识别扩展为可区分有效槽位、池内无效地址和非池内地址。
4. **`engine/value_buffer.h` / `engine/value_buffer.cpp`**（如需要，修改）：
   保持公开 API 不变；必要时只调整内部控制块与池识别数据。
5. **新增内部路径选择结构**（文件位置可在实现时确定，推荐放在 `engine/put_path.h` 或 reactor 私有头）：
   `PutValueSource`、`PutWritePlan`、后端直接写入能力判断函数。
6. **`test/engine/*`**（修改 / 新增）：
   增加路径选择单元测试、键绑定测试、端到端行为测试、多设备错配测试和并发测试。
7. **`doc/P8/P8M3_put_path_design.md`**（本文）与 `doc/P8/README.md`（更新）：
   固定 P8M3 设计边界和术语。

### 1.3 明确不做

| 不做项 | 归属 |
|---|---|
| 修改公开 `Put` 签名 | 不做 |
| 新增 `Put(ValueBuffer&&)` 或“强制零拷贝”接口 | 不做 |
| 新增公开路径统计接口 | 不做 |
| `IoBackend` 写入描述符升级 | P8M4 |
| `io_uring` 注册缓冲区注册、注销和 fixed buffer 写入 | P8M4 |
| SPDK 后端或 SPDK DMA 内存接入 | P10 |
| bench 和性能档案归档 | P8M5 |
| 写入流水线化 | 后续性能阶段 |
| 通用 `BufferHandle` 基础设施 | 不做；当前仍是 value 专用 `ValueBuffer` |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 |
|---|---|---|
| **P8M3-D1** | M3 边界 | P8M3 只做 `Put` 路径选择和直接写入路径；合格 value 跳过 `BufferPool + memcpy`，直接调用现有 `IoBackend::Write`；不升级后端协议。 |
| **P8M3-D2** | 路径选择位置 | 采用两段式：`Engine::Put` 做目标设备路由和全局来源预识别；`Reactor::ExecutePut` 做最终写入计划和执行。 |
| **P8M3-D3** | 内部分类结构 | 新增内部 `PutValueSource` / `PutWritePlan` 等结构；不进入公开 API。 |
| **P8M3-D4** | Cabe 值缓冲区匹配规则 | `ValueBuffer` 采用键绑定：只有分配键与 `Put` 键完全一致，且槽位有效、设备归属匹配，才进入主直接写入路径。 |
| **P8M3-D5** | 键不匹配 / 跨设备行为 | 键不匹配和设备不匹配都不返回错误；有效 Cabe 值缓冲区不满足主路径条件时复制回退。 |
| **P8M3-D6** | 池内无效地址 | 地址落在值缓冲区池内但槽位未分配或不完整时，分类为 `PoolAddressNotAllocated`，复制回退；文档标记为生命周期误用。 |
| **P8M3-D7** | 应用端自备内存规则 | 采用后端能力判断；P8M3 当前 sync / `io_uring` 普通写按 4 KiB 对齐判断；未来 SPDK 必须是 Cabe 可验证的 DMA 可用内存。 |
| **P8M3-D8** | 后端接口 | P8M3 复用现有 `IoBackend::Write(block_idx, const std::byte*)`；写入描述符留给 P8M4。 |
| **P8M3-D9** | 写入顺序 | 不改变 `ExecutePut` 事务顺序；只把 “BufferPool + memcpy” 替换为“写入源选择”。 |
| **P8M3-D10** | CRC 与生命周期 | `Put` 调用期间 value 必须存活且内容不可变；CRC 基于实际写入源计算；Cabe 不做运行时防护。 |
| **P8M3-D11** | `BufferPool` 角色 | `BufferPool` 继续保留，服务复制回退路径和 `Get` 临时读缓冲；不与 `ValueBufferPool` 合并。 |
| **P8M3-D12** | 错误语义 | 未命中直接写入路径不是错误；复制回退对公开 `Put` 透明；不新增公开路径统计接口。 |
| **P8M3-D13** | 可测试性 | 抽出内部路径选择纯函数做单元测试；端到端测试验证结果一致；不引入测试专用生产 hook。 |
| **P8M3-D14** | 测试矩阵 | 覆盖键绑定、回退、外部对齐内存、跨设备、并发、TSAN、sync / `io_uring` 回归；bench 留 P8M5。 |

---

## 3. 核心模型

### 3.1 两类直接写入

P8M3 中“直接写入”只表示 Cabe 不再复制 value 到 reactor 内部临时缓冲区：

```text
直接写入:
   value.data() -> IoBackend::Write(block_idx, value.data())

复制回退:
   value.data() -> memcpy -> BufferPool buffer -> IoBackend::Write(block_idx, buffer)
```

它不等价于 P8M4 的 `io_uring` 注册缓冲区写入，也不等价于未来 SPDK DMA 写入。

| 路径 | value 来源 | P8M3 条件 | P8M3 后端调用 |
|---|---|---|---|
| 主直接写入路径 | Cabe 值缓冲区 | 绑定键匹配、槽位有效、设备归属匹配 | `IoBackend::Write(block, value.data())` |
| 条件式直接写入路径 | 应用端自备值内存 | 当前后端能力判断通过 | `IoBackend::Write(block, value.data())` |
| 复制回退路径 | 任意 value | 不满足直接写入条件 | `memcpy` 到 `BufferPool` 后 `Write` |

### 3.2 键绑定

P8M3 将 P8M1/P8M2 中“分配 key 只用于路由”的语义收紧为键绑定：

```text
AllocateValueBuffer(key)
  -> 记录 bound_key = key
  -> 记录 owner_device = RouteKey(key)
  -> 从 owner_device 的 ValueBufferPool 分配槽位

Put(key, buffer.view())
  -> 只有 key == bound_key 时，才允许命中主直接写入路径
```

示例：

```text
AllocateValueBuffer("a") -> 设备 0
Put("a", view)           -> 主直接写入候选
Put("b", view)           -> 复制回退，即使 "b" 也路由到设备 0
```

键绑定是性能路径契约，不是公开写入正确性契约。键不匹配不返回错误。

### 3.3 两段式路径选择

P8M3 采用两段式：

```text
Engine::Put
  -> 校验 key/value
  -> 计算 target_device = RouteKey(key)
  -> 扫描所有 ValueBufferPool
  -> 生成 PutValueSource
  -> 投递目标 reactor

Reactor::ExecutePut
  -> 根据 PutValueSource 和后端能力生成 PutWritePlan
  -> 执行直接写入或复制回退
```

原因：

1. `Engine` 已持有所有设备池，可以识别跨设备 `ValueBuffer`。
2. 目标 `reactor` 只持有本设备池，若只在 `reactor` 内识别，会把其他设备池的 `ValueBuffer` 误判为普通外部内存。
3. 写入执行、block 分配、WAL、索引和回收仍属于 `reactor`，符合 P7 之后的结构。

### 3.4 `DataView` 身份边界

`DataView` 只有地址和长度，不携带 `ValueBuffer` 控制块。P8M3 只能通过池地址范围、槽位状态、槽位代际和绑定键做识别。

如果调用方在 `ValueBuffer::reset()` 或析构后继续使用旧 `DataView`，这是非法生命周期用法。P8M3 为保持
`Put` 统一语义，仍复制回退，但不承诺这种用法是合法推荐路径。

---

## 4. 内部结构

### 4.1 `PutValueSource`

推荐内部结构：

```cpp
enum class PutValueSourceKind : std::uint8_t {
    ExternalValueMemory = 0,
    TargetKeyValueBuffer = 1,
    OtherKeyValueBuffer = 2,
    OtherDeviceValueBuffer = 3,
    PoolAddressNotAllocated = 4,
};

struct PutValueSource {
    PutValueSourceKind kind = PutValueSourceKind::ExternalValueMemory;
    DeviceId owner_device_id = 0;
    std::uint64_t pool_id = 0;
    std::uint32_t slot_index = 0;
    std::uint32_t slot_generation = 0;
};
```

含义：

| `kind` | 含义 | P8M3 行为 |
|---|---|---|
| `TargetKeyValueBuffer` | value 来自 Cabe 值缓冲区，绑定键与 `Put` 键一致，设备归属匹配 | 主直接写入 |
| `OtherKeyValueBuffer` | value 来自目标设备池，但绑定键与 `Put` 键不一致 | 复制回退 |
| `OtherDeviceValueBuffer` | value 来自其他设备池 | 复制回退 |
| `PoolAddressNotAllocated` | 地址落在池内，但不是当前有效完整已分配槽位 | 复制回退 |
| `ExternalValueMemory` | value 不属于任何 Cabe 值缓冲区池 | 后端能力判断，通过则条件式直接写入，否则复制回退 |

`PutValueSource` 是内部结构，不进入公开 API。

### 4.2 `PutWritePlan`

推荐内部结构：

```cpp
enum class PutWritePath : std::uint8_t {
    Direct = 0,
    CopyFallback = 1,
};

struct PutWritePlan {
    PutWritePath path = PutWritePath::CopyFallback;
};
```

推荐函数：

```cpp
PutWritePlan BuildPutWritePlan(DataView value,
                                const PutValueSource& source,
                                const BackendDirectWriteCaps& caps) noexcept;
```

P8M3 中，`BackendDirectWriteCaps` 可以很小，只表达当前后端普通写入是否允许外部内存直接写入，以及要求的
地址对齐。实现位置可在代码阶段根据 include 依赖确定。

### 4.3 后端能力判断

P8M3 不把应用端自备内存规则写死为某个永久对齐值，而是写成后端能力判断：

```text
ExternalValueMemory:
  if 当前后端认为该 DataView 可直接写入:
      条件式直接写入
  else:
      复制回退
```

P8M3 当前实现：

| 后端 | 条件 |
|---|---|
| sync | value 地址满足 4 KiB 对齐 |
| `io_uring` 普通写 | value 地址满足 4 KiB 对齐 |

未来：

| 后端 | 条件 |
|---|---|
| `io_uring` 注册缓冲区 | value 必须是已注册缓冲区或可映射到注册缓冲区描述符 |
| SPDK | value 必须来自 Cabe 可验证的 DMA 可用内存 |

1 MiB 对齐仍是 Cabe 值缓冲区槽位布局要求，不是应用端自备内存的通用 SPDK 规则。

---

## 5. `ValueBufferPool` 扩展

### 5.1 绑定键存储

P8M3 需要在槽位元数据中保存绑定键。由于 `kWalKeyMax` 很小，推荐使用固定长度存储，避免每槽位
`std::string` 产生堆分配：

```text
Slot
  next_free
  state
  generation
  data_address
  bound_key_len
  bound_key_bytes[kWalKeyMax]
```

分配流程变更：

```text
Engine::AllocateValueBuffer(key)
  -> key 已通过校验
  -> target pool Allocate(bound_key = key)
  -> 槽位记录 bound_key
  -> 返回 ValueBuffer
```

释放后槽位状态变为 Free。绑定键可以清零，也可以在状态为 Free 时视为无效；来源识别必须以槽位状态为准。

### 5.2 来源探测结果

当前 P8M2 的 `Identify(DataView)` 只能表达匹配 / 不匹配。P8M3 需要更细的内部探测能力：

```cpp
enum class ValueBufferProbeKind : std::uint8_t {
    NotInPool = 0,
    AllocatedSlot = 1,
    PoolAddressNotAllocated = 2,
};

struct ValueBufferProbe {
    ValueBufferProbeKind kind = ValueBufferProbeKind::NotInPool;
    DeviceId device_id = 0;
    std::uint64_t pool_id = 0;
    std::uint32_t slot_index = 0;
    std::uint32_t slot_generation = 0;
    std::string_view bound_key;
};
```

实际实现不一定使用 `std::string_view` 暴露槽位内部地址，也可以提供比较函数：

```cpp
bool bound_key_equals(std::string_view key) const noexcept;
```

关键是 `Engine::Put` 至少要能区分：

1. 不属于任何池；
2. 属于某个池且当前是有效已分配完整槽位；
3. 属于某个池地址范围，但不是有效已分配完整槽位。

### 5.3 全局预识别

`Engine::Put` 扫描 `value_buffer_pools_`：

```text
for pool in value_buffer_pools_:
    probe = pool->Probe(value)
    if probe.kind == AllocatedSlot:
        if probe.device_id != target_device:
            return OtherDeviceValueBuffer
        if bound_key != Put key:
            return OtherKeyValueBuffer
        return TargetKeyValueBuffer
    if probe.kind == PoolAddressNotAllocated:
        remember PoolAddressNotAllocated

if remembered:
    return PoolAddressNotAllocated
return ExternalValueMemory
```

如果多个池理论上都声称匹配，属于内部不变量破坏；正常实现下各池 slab 不重叠。

扫描是 O(N)，N 最大 256。P8M3 先接受简单实现；如果未来需要优化，可建立地址区间索引。

---

## 6. `Engine::Put` 集成

P8M3 后 `Engine::Put` 逻辑：

```text
Status Engine::Put(key, value)
  if !opened_: kEngineNotOpen
  if key.empty(): kMemEmptyKey
  if value.size() != kValueSize: kEngineInvalidValue
  if key.size() > kWalKeyMax: kWalKeyTooLong

  target = RouteKey(key)
  source = ClassifyPutValueSource(target, key, value)

  OpNode op
    type = Put
    key = key
    value = value
    value_source = source

  rc = SubmitAndWait(*reactors_[target], op)
  return Status from rc
```

说明：

- `Engine::Put` 仍不执行 I/O，不分配 block，不写 WAL。
- 来源预识别必须在投递前完成，因为只有 `Engine` 持有全部设备池。
- `OpNode` 仍为调用线程栈对象；`Put` 同步等待，`key` 和 `value` 视图在调用期间有效。

---

## 7. `Reactor::ExecutePut` 集成

### 7.1 总流程

P8M3 后的写入流程：

```text
ExecutePut(op)
  -> block_allocator.Acquire(new block)
  -> BuildPutWritePlan(op->value, op->value_source, backend caps)
  -> 准备写入源
       Direct:
         source = op->value.data()
       CopyFallback:
         buf = dc_.pool.Allocate()
         memcpy(op->value -> buf)
         source = buf
  -> CRC32(source)
  -> dc_.io.Write(block, source)
  -> 写 WAL
  -> 更新 meta index
  -> 覆盖写时回收旧 block
  -> MaybeRequestSnapshot
  -> 释放复制回退临时缓冲区
```

### 7.2 直接写入路径

```text
source = op->value.data()
crc = CRC32(DataView{source, kValueSize})
rc = dc_.io.Write(block_id.block_idx(), source)
```

直接写入路径不使用 `dc_.pool.Allocate()`。

### 7.3 复制回退路径

```text
buf = dc_.pool.Allocate()
if buf == nullptr:
    recycle new block
    return kEnginePoolExhausted

memcpy(buf, op->value.data(), kValueSize)
source = buf
crc = CRC32(DataView{buf, kValueSize})
rc = dc_.io.Write(block_id.block_idx(), buf)
dc_.pool.Free(buf)
```

建议实现一个小型内部 RAII 辅助，确保所有失败路径释放 `BufferPool` 临时缓冲区。该辅助不需要公开。

### 7.4 事务顺序不变

P8M3 必须保持：

```text
申请新 block
写 value
写 WAL
更新 index
回收旧 block
MaybeRequestSnapshot
```

失败回收规则：

| 失败点 | 处理 |
|---|---|
| `block_allocator.Acquire` 失败 | 直接返回错误 |
| 复制回退 `BufferPool` 分配失败 | 回收新 block，返回 `kEnginePoolExhausted` |
| `io.Write` 失败 | 释放临时缓冲区，回收新 block，返回 I/O 错误 |
| `WriteWalRescuing` 失败 | 回收新 block，返回错误 |
| 覆盖旧 key | 新 meta 生效后再回收旧 block |

`WAL` 成功前不更新索引；新索引生效前不回收旧 block。

---

## 8. 生命周期与并发约束

### 8.1 `Put` 期间 value 必须稳定

P8M3 后直接写入路径依赖调用方遵守：

```text
从 Put 调用开始到 Put 返回之前，value 指向的内存必须存活且内容不可变。
```

适用于所有来源：

- `std::vector<std::byte>`；
- `posix_memalign` / `aligned_alloc` 自备内存；
- Cabe `ValueBuffer`。

合法：

```cpp
auto r = engine.AllocateValueBuffer("k");
std::memset(r.buffer.data().data(), 0xA7, cabe::kValueSize);

auto st = engine.Put("k", r.buffer.view());
r.buffer.reset();
```

非法：

```cpp
auto r = engine.AllocateValueBuffer("k");

std::thread t([&] {
    engine.Put("k", r.buffer.view());
});

std::memset(r.buffer.data().data(), 0x00, cabe::kValueSize);
t.join();
```

若调用方违反约束，Cabe 不保证结果正确，可能表现为 CRC 校验失败或写入内容撕裂。

### 8.2 陈旧 `DataView`

以下不是合法推荐用法：

```cpp
auto r = engine.AllocateValueBuffer("k");
auto view = r.buffer.view();
r.buffer.reset();

engine.Put("k", view);
```

P8M3 中这类地址如果还能被识别为池内无效地址，则复制回退；如果槽位已被重新分配，Cabe 只能看到当前槽位状态，
无法从 `DataView` 区分旧 view 与新所有者。文档必须明确：`DataView` 生命周期不得超过生成它的 `ValueBuffer`。

---

## 9. 错误语义

P8M3 不新增错误码。

| 场景 | 行为 |
|---|---|
| Engine 未打开 | `kEngineNotOpen` |
| key 为空 | `kMemEmptyKey` |
| key 过长 | `kWalKeyTooLong` |
| value 大小不是 1 MiB | `kEngineInvalidValue` |
| 键不匹配 `ValueBuffer` | 复制回退，不报错 |
| 跨设备 `ValueBuffer` | 复制回退，不报错 |
| 池内无效地址 | 复制回退，不报错 |
| 应用端自备内存不满足后端直接写入能力 | 复制回退，不报错 |
| 复制回退 `BufferPool` 耗尽 | `kEnginePoolExhausted` |
| 数据盘无空间 | `kEngineNoSpace` |
| I/O 失败 | 后端既有错误 |
| WAL 写失败 | WAL 既有错误 |

路径选择对公开 `Put` 透明。没有命中直接写入不是错误。

---

## 10. 与既有组件的关系

### 10.1 与 `BufferPool`

`BufferPool` 继续保留：

```text
Get:
  io.Read -> BufferPool buffer -> memcpy 到 caller out

Put 复制回退:
  caller value -> memcpy -> BufferPool buffer -> io.Write
```

主直接写入路径和条件式直接写入路径不使用 `BufferPool`。

不合并 `BufferPool` 与 `ValueBufferPool`，原因：

- `BufferPool` 是 reactor 私有、单线程内部资源；
- `ValueBufferPool` 是应用可间接获得的跨线程资源；
- `ValueBufferPool` 需要来源识别、绑定键、关闭等待和槽位代际；
- `BufferPool` 不需要这些复杂语义。

### 10.2 与 `IoBackend`

P8M3 继续调用：

```cpp
int32_t Write(std::uint64_t block_idx, const std::byte* buf);
```

不改 `IoBackend` concept，不改 sync / `io_uring` 后端签名。

P8M4 再升级为写入描述符，表达：

- 数据地址；
- 来源类型；
- 设备归属；
- 池编号；
- 槽位编号；
- 注册缓冲区索引；
- 后端私有信息。

### 10.3 与 SPDK

P8M3 不引入 SPDK。但 D7 已为 SPDK 留出边界：

```text
应用端自备普通内存即使 1 MiB 对齐，也不自动成为 SPDK 直接写入候选。
```

未来 SPDK 直接写入要求内存来自 Cabe 可验证的 DMA 可用来源。P10 可以把 `ValueBufferPool` 底层分配替换为
SPDK / DPDK DMA 内存池，而不推翻 `ValueBuffer` 公开语义。

---

## 11. 实施顺序

推荐按以下顺序实现：

1. 扩展 `ValueBufferPool` 槽位元数据，分配时记录绑定键。
2. 增加池内部来源探测能力，区分有效槽位、池内无效地址和非池内地址。
3. 新增 `PutValueSource`、`PutWritePlan` 和后端能力判断函数。
4. 在 `Engine::Put` 中实现全局来源预识别，并把结果填入 `OpNode`。
5. 在 `Reactor::ExecutePut` 中接入 `PutWritePlan`。
6. 重构复制回退临时缓冲区释放，避免直接路径和回退路径分支增加后漏释放。
7. 补齐内部路径选择单测。
8. 补齐 `ValueBufferPool` 键绑定测试。
9. 补齐 Engine 端到端路径测试、多设备错配测试和并发测试。
10. 跑 sync / `io_uring` 回归矩阵。

---

## 12. 测试计划

### 12.1 内部路径选择单元测试

| 用例 | 输入 | 预期 |
|---|---|---|
| `TargetKeyValueBufferDirect` | `TargetKeyValueBuffer` | `Direct` |
| `OtherKeyValueBufferFallback` | `OtherKeyValueBuffer` | `CopyFallback` |
| `OtherDeviceValueBufferFallback` | `OtherDeviceValueBuffer` | `CopyFallback` |
| `PoolAddressNotAllocatedFallback` | `PoolAddressNotAllocated` | `CopyFallback` |
| `ExternalAlignedDirect` | 外部内存 + 4 KiB 对齐 + 当前后端允许 | `Direct` |
| `ExternalUnalignedFallback` | 外部内存 + 非 4 KiB 对齐 | `CopyFallback` |

### 12.2 `ValueBufferPool` 增量测试

| 用例 | 内容 |
|---|---|
| `IdentifyCarriesBoundKey` | 分配时保存绑定 key，识别时可用于比较。 |
| `SameKeyMatchesTargetKey` | 分配 key 与 `Put` key 一致时命中主路径分类。 |
| `DifferentKeySameDeviceFallback` | 不同 key 即使同设备也复制回退。 |
| `KeyStorageMaxLength` | `kWalKeyMax` 长度 key 可正确绑定与比较。 |
| `ReleasedSlotDoesNotMatchBoundKey` | 释放后不再命中已分配槽位。 |
| `ReusedSlotReplacesBoundKey` | 槽位复用后绑定 key 更新。 |

### 12.3 Engine 端到端测试

| 用例 | 输入 | 预期 |
|---|---|---|
| `VectorValueFallbackRoundTrip` | 普通 `std::vector<std::byte>` | Put/Get 成功 |
| `AlignedExternalValueRoundTrip` | `posix_memalign(4096, 1 MiB)` | Put/Get 成功 |
| `ValueBufferSameKeyRoundTrip` | `AllocateValueBuffer(k)` + `Put(k)` | Put/Get 成功 |
| `ValueBufferDifferentKeyFallback` | `AllocateValueBuffer(a)` + `Put(b)` | Put/Get 成功 |
| `ValueBufferOtherDeviceFallback` | 设备 0 分配、设备 1 key 写入 | Put/Get 成功 |
| `ReleasedValueBufferViewFallback` | `reset()` 后旧 view | 按 D6 复制回退，行为测试记录为非法生命周期场景 |
| `ConcurrentMixedPut` | 多线程混合普通 value、对齐外部内存和 `ValueBuffer` | 正确、不挂、TSAN 无数据竞争 |

### 12.4 回归矩阵

P8M3 完成后建议至少跑：

```bash
./scripts/run-tests.sh --backend=sync --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=...
./scripts/run-tests.sh --backend=sync --tsan --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --filter 'ValueBuffer|PutPath|Engine'
./scripts/run-tests.sh --backend=sync --asan --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --filter 'ValueBuffer|PutPath|Engine'
./scripts/run-tests.sh --backend=sync --ubsan --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --filter 'ValueBuffer|PutPath|Engine'
./scripts/run-tests.sh --backend=io_uring --release --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --filter 'ValueBuffer|PutPath|Engine'
./scripts/run-tests.sh --backend=io_uring --asan --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --filter 'ValueBuffer|PutPath|Engine'
./scripts/run-tests.sh --backend=io_uring --ubsan --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --filter 'ValueBuffer|PutPath|Engine'
```

全量合入前建议：

```bash
./scripts/run-tests.sh --backend=sync --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=...
./scripts/run-coverage.sh --backend=sync --device=... --wal-device=... --snapshot-device=... --device2=... --wal-device2=... --snapshot-device2=... --strict
```

`io_uring + TSAN` 继续按项目既有规则排除。

---

## 13. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 跨设备 `ValueBuffer` 被误判为外部内存 | 后续注册缓冲区或 SPDK 下可能错误使用其他设备池内存 | `Engine::Put` 扫描全部池，显式分类为 `OtherDeviceValueBuffer`。 |
| 键不匹配仍进入主路径 | 违反键绑定契约，调用方误用难以发现 | 槽位记录绑定键；路径选择要求绑定键与 `Put` 键完全一致。 |
| 池内无效地址被当成合法外部内存直接写 | 陈旧 `DataView` 可能绕过生命周期约束 | 分类为 `PoolAddressNotAllocated`，复制回退，不进入条件式直接写入。 |
| 应用端并发修改 value | 直接写入路径可能出现 CRC 与盘上内容不一致 | 文档明确 `Put` 期间 value 必须存活且内容不可变；Cabe 不做运行时防护。 |
| 复制回退临时缓冲区泄漏 | 失败路径增多后资源未释放 | 使用局部 RAII 或集中释放逻辑。 |
| 写入事务顺序被破坏 | WAL / index / block 回收语义退化 | P8M3 只替换写入源选择，保持现有顺序和失败回收规则。 |
| 后端能力判断写死 | 未来 SPDK 被 4 KiB / 1 MiB 对齐规则绑住 | 通过后端能力判断函数表达；SPDK 规则后续替换为 DMA 可用内存验证。 |
| 内部路径不可证明 | 只靠 Put/Get 无法确认是否直接写入 | 抽出内部路径选择纯函数并单测；不新增公开统计接口。 |

---

## 14. 退出条件

P8M3 完成时必须满足：

1. `Engine::Put` 公开签名不变。
2. `Engine::Put` 能完成目标设备路由和全局值内存来源预识别。
3. `OpNode` 能携带内部 `PutValueSource`。
4. `ValueBufferPool` 槽位能记录并比较绑定键。
5. 分配键与 `Put` 键一致时，Cabe 值缓冲区可进入主直接写入路径。
6. 分配键与 `Put` 键不一致时，不报错并复制回退。
7. 跨设备 `ValueBuffer` 不报错并复制回退。
8. 池内无效地址不进入直接写入路径。
9. 应用端自备 4 KiB 对齐内存在 sync / `io_uring` 普通写后端下可进入条件式直接写入路径。
10. 应用端普通未对齐内存复制回退。
11. 直接写入路径不分配 `BufferPool`。
12. 复制回退路径继续使用 `BufferPool`。
13. `Get` 路径不变。
14. `ExecutePut` 的 WAL、CRC、索引更新和旧 block 回收顺序不变。
15. 不新增公开路径统计接口。
16. 不升级 `IoBackend` 写入协议。
17. 内部路径选择单元测试覆盖 D13 规则。
18. Engine 端到端测试覆盖普通 value、对齐外部内存、同 key `ValueBuffer`、不同 key `ValueBuffer`、跨设备 `ValueBuffer`。
19. sync 全量测试通过。
20. sync + TSAN 下 P8M3 并发测试无数据竞争。
21. `io_uring` release / ASAN / UBSAN 相关测试通过。

---

## 15. 后续衔接

P8M3 完成后，P8M4 可以在不改变公开 `Put` 语义的前提下，把 P8M3 的写入计划继续下沉为后端写入描述符：

```text
P8M3:
  PutValueSource -> PutWritePlan -> IoBackend::Write(block, const byte*)

P8M4:
  PutValueSource -> WriteBuffer 描述符 -> 后端普通写 / 注册缓冲区写
```

P8M4 应复用 P8M3 的来源分类结果，把 `TargetKeyValueBuffer` 映射到 `io_uring` 注册缓冲区槽位，并继续让
`OtherKeyValueBuffer`、`OtherDeviceValueBuffer`、`PoolAddressNotAllocated` 和不满足后端能力的外部内存走复制回退。

P10 SPDK 后端应把 Cabe 值缓冲区池底层分配替换为 DMA 可用内存，应用端自备普通内存即使对齐也不自动进入
SPDK 直接写入。
