# Cabe P8-M2 设计：`ValueBufferPool` 与无锁值缓冲区池

> 本里程碑把 P8M1 已落地的 `ValueBuffer` 公开接口推进为真实可用的值缓冲区分配能力：
> 每个数据设备拥有独立的 `ValueBufferPool`，池内管理固定 1 MiB、1 MiB 对齐的 value 槽位。
> `Engine::AllocateValueBuffer(key)` 在调用线程内完成 key 校验、key 路由和池分配，返回可由应用填充的
> `ValueBuffer`。P8M2 直接实现无锁分配 / 释放 / 关闭等待，不引入临时 mutex 方案。
>
> P8M2 **不改变 `Put` 写入行为**。调用方把 `ValueBuffer.view()` 传给 `Put` 后，仍按现有 P7 写路径复制到
> reactor 私有 `BufferPool` 后写入设备。真正的零拷贝路径选择和后端写入协议升级分别留给 P8M3 / P8M4。
>
> **本文为详细设计**，汇总 P8M2-D1 ~ P8M2-D14 的全部裁决。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P8 / M2 |
| 状态 | ✅ 已实装 |
| 上游依赖 | P1 ~ P7 已完成；P8M1 已实装；P8 阶段总计划见 `doc/P8/README.md` |
| 下游依赖本里程碑 | P8M3 `Put` 路径选择；P8M4 后端写入协议与 `io_uring` 注册缓冲区；P8M5 bench 与收敛 |
| 退出判定 | 见 §13 |

---

## 1. 目标与范围

### 1.1 目标

1. **真实分配 `ValueBuffer`**：`Engine::AllocateValueBuffer(key)` 在合法 key 下返回有效 `ValueBuffer`。
2. **每设备值缓冲区池**：每个数据设备一个独立 `ValueBufferPool`，容量由
   `Options::value_buffer_pool_blocks` 控制。
3. **固定 1 MiB 对齐槽位**：池在打开阶段分配一整块连续 slab，基础地址 1 MiB 对齐，按 1 MiB 切分槽位。
4. **无锁分配与释放**：池内部使用带版本号的原子空闲栈管理槽位，不使用 mutex / condition_variable。
5. **严格关闭边界**：`Engine::Close()` 必须等待所有已分配 `ValueBuffer` 释放后才继续关闭设备并返回。
6. **来源识别基础能力**：池能判断一个 `DataView` 是否来自当前设备池中处于已分配状态的完整槽位。
7. **后端中立**：P8M2 不绑定 `io_uring` 注册缓冲区或 SPDK，但池元数据为未来后端私有信息预留空间。
8. **保持 `Put` 统一入口**：`Put` 的公开接口和实际写入行为在 P8M2 不改变。

### 1.2 交付范围

1. **`engine/value_buffer_pool.h` / `engine/value_buffer_pool.cpp`**（新建）：
   `ValueBufferPool`、池创建结果、来源识别结果、无锁槽位管理、关闭等待。
2. **`engine/value_buffer.h` / `engine/value_buffer.cpp`**（修改）：
   接入真实控制块；新增 `ValueBuffer::reset()`；移动赋值时正确释放旧槽位。
3. **`engine/device_context.h`**（修改）：
   `DeviceContext` 持有本设备的 `ValueBufferPool` 引用，使后续 `reactor` 能识别本设备池。
4. **`engine/engine.h` / `engine/engine.cpp`**（修改）：
   `Engine` 保存按路由可达的每设备池引用；`Open` 创建池；`AllocateValueBuffer` 路由并分配；
   `Close` 标记池关闭并等待所有 `ValueBuffer` 释放。
5. **`engine/CMakeLists.txt`**（修改）：
   将 `value_buffer_pool.cpp` 加入 `cabe_engine`。
6. **`test/engine/value_buffer_pool_test.cpp`**（新建）：
   覆盖池单测、无锁空闲栈、对齐、耗尽、释放复用、来源识别和关闭等待。
7. **`test/engine/value_buffer_test.cpp`**（修改）：
   将 P8M1 占位测试更新为真实分配行为；补充 `reset()`、move、Engine 集成、多设备和 `Put/Get` 联动。
8. **`test/CMakeLists.txt`**（修改）：
   注册新增测试目标。
9. **`doc/P8/P8M2_value_buffer_pool_design.md`**（本文）与 `doc/P8/README.md`（更新）：
   固定 P8M2 设计边界。

### 1.3 明确不做

| 不做项 | 归属 |
|---|---|
| 修改 `Reactor::ExecutePut` 的零拷贝路径选择 | P8M3 |
| 把来源识别接入 `Put` 行为分支 | P8M3 |
| `IoBackend::Write` 写入描述符升级 | P8M4 |
| `io_uring` 注册缓冲区注册、注销和写入 | P8M4 |
| SPDK 后端或 SPDK 大页内存接入 | P10 |
| 应用端跨进程导入外部内存 | 不做 |
| 新增公开“强制零拷贝”写入接口 | 不做 |
| 新增公开路径统计接口 | 不做 |
| bench | P8M5 |
| 通用 `BufferHandle` 基础设施 | 不做；当前只做 value 专用 `ValueBuffer` |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 |
|---|---|---|
| **P8M2-D1** | 池归属 | 每个设备一个独立 `ValueBufferPool`；`Engine` 保存路由可达的池引用；对应设备的 `DeviceContext` / `reactor` 也持有同一池引用。 |
| **P8M2-D2** | 公开分配是否进入 reactor | `AllocateValueBuffer(key)` 不投递到 `reactor`；调用线程内完成 key 校验、key 路由和池分配。 |
| **P8M2-D3** | 是否复用现有 `BufferPool` | 不复用。`BufferPool` 继续服务 reactor 内部复制路径；P8 新增 value 专用 `ValueBufferPool`。 |
| **P8M2-D4** | 内存形态 | 每设备一块连续 slab，基础地址 1 MiB 对齐，按 1 MiB 切成固定槽位。 |
| **P8M2-D5** | 来源识别规则 | 严格识别：地址等于目标设备池槽位起点，长度等于 1 MiB，槽位处于已分配状态，设备归属匹配 key 路由。 |
| **P8M2-D6** | 释放路径 | `ValueBuffer` 通过 RAII 归还槽位；提供 `reset()` 显式释放；move 后只释放一次。 |
| **P8M2-D7** | `Close/Open` 边界 | 采用严格模式：`ValueBuffer` 不允许跨 `Close/Open` 存活；`Close()` 等待所有已分配 `ValueBuffer` 释放后才返回。 |
| **P8M2-D8** | 并发模型 | `ValueBufferPool` 分配和释放采用无锁设计：带版本号原子空闲栈 + 原子计数 + `atomic::wait/notify`。 |
| **P8M2-D9** | 容量为 0 | `value_buffer_pool_blocks = 0` 合法；`Open()` 成功；`AllocateValueBuffer()` 返回 `kEnginePoolExhausted`；普通 `Put` 不受影响。 |
| **P8M2-D10** | slab 分配失败 | 当容量 > 0 时，任一设备 slab 分配失败则 `Open()` 失败并回滚，不静默降级、不懒分配。 |
| **P8M2-D11** | 元数据 | 池保存设备身份、池身份、地址范围、槽位数量、关闭状态、无锁栈状态和后端预留字段；槽位保存编号、状态、代际和空闲链指针。 |
| **P8M2-D12** | 是否修改 `Put` | P8M2 不改变 `Put` 写入行为；`ValueBuffer.view()` 仍按普通 `DataView` 复制写入。 |
| **P8M2-D13** | 测试范围 | 覆盖池单测、Engine 集成、`ValueBuffer + Put/Get`、多线程分配释放、严格 `Close()`、容量 0、池耗尽、多设备独立性和 TSAN。 |
| **P8M2-D14** | 命名 | 当前是 value 专用缓冲区，统一使用 `ValueBuffer` / `ValueBufferPool`；不使用 `BufferHandle`。 |

---

## 3. 核心模型

### 3.1 每设备一个值缓冲区池

P8M2 的池归属与 P7M4 的设备路由保持一致：

```text
key
  -> RouteKey(key)
  -> device i
  -> ValueBufferPool i
  -> ValueBuffer(slot)
```

多设备形态：

```text
Engine
  ├─ reactors_[0] -> DeviceContext 0 -> ValueBufferPool 0
  ├─ reactors_[1] -> DeviceContext 1 -> ValueBufferPool 1
  └─ value_buffer_pools_ 可按 RouteKey(key) 直接访问同一组池
```

`Engine::AllocateValueBuffer(key)` 需要在调用线程内路由到目标设备，因此 `Engine` 必须持有一份池引用。
后续 `reactor` 写入执行层需要识别 `DataView` 是否来自本设备池，因此 `DeviceContext` 也必须持有同一池引用。

### 3.2 `ValueBuffer` 是 value 专用对象

P8M2 的 `ValueBuffer` 不是通用 buffer 基础设施：

| 特征 | `ValueBuffer` |
|---|---|
| 用途 | 专门承载 key/value 中固定 1 MiB 的 value |
| 大小 | 固定 `kValueSize` |
| 分配依据 | key 路由到目标设备池 |
| 提交方式 | 仍通过 `Engine::Put(key, buffer.view())` |
| 关闭边界 | 必须在分配它的 Engine 打开周期内释放 |
| 后端信息 | 不暴露给上层应用 |

`BufferHandle` 这种通用命名保留给未来真正跨 value、WAL、snapshot、读缓存等模块的底层内存基础设施。
P8M2 不提前引入。

### 3.3 严格 Engine 打开周期

P8M2 将 P8M1 的生命周期边界收紧为严格模式：

```text
Engine::Open 成功
  -> 可 AllocateValueBuffer
  -> ValueBuffer 可被应用填充和用于 Put
Engine::Close 开始
  -> 拒绝新的 AllocateValueBuffer
  -> 等待所有已分配 ValueBuffer 释放
  -> 停止 reactor
  -> 关闭 snapshot / wal / data 设备
Engine::Close 返回
  -> 不存在旧周期 ValueBuffer
  -> 下一次 Open 是干净的新周期
```

P7M3 已经定义 `Open` / `Close` 是排他操作：调用方必须保证 `Open` / `Close` 执行期间没有其他 Engine
方法调用与之重叠。P8M2 不改变这个契约；它只额外要求 `Close()` 等待此前合法分配、但尚未释放的
`ValueBuffer`。

### 3.4 `DataView` 的身份边界

`DataView` 只携带地址和长度，不携带 `ValueBuffer` 控制块身份。因此 P8M2 的来源识别能力是：

```text
地址是否等于当前池某个槽位起点
长度是否等于 1 MiB
该槽位当前是否处于 allocated 状态
设备归属是否匹配
```

如果上层在 `ValueBuffer::reset()` 或析构后继续保存旧 `DataView` 并使用，这是调用方违反生命周期契约。
当槽位已释放且尚未重新分配时，来源识别应失败；如果同一槽位已被重新分配给新的 `ValueBuffer`，旧
`DataView` 与新所有者别名，Cabe 仅能看到当前地址属于已分配槽位。P8 文档必须明确：`DataView` 的有效期
不得超过生成它的 `ValueBuffer` 的有效期。

---

## 4. 主要结构

### 4.1 `ValueBufferPool`

建议头文件形态：

```cpp
class ValueBufferPool {
public:
    struct CreateResult {
        Status status;
        std::shared_ptr<ValueBufferPool> pool;
        bool ok() const noexcept;
    };

    struct SourceInfo {
        bool matched = false;
        DeviceId device_id = 0;
        std::uint64_t pool_id = 0;
        std::uint32_t slot_index = 0;
        std::uint32_t slot_generation = 0;
    };

    static CreateResult Create(DeviceId device_id,
                               std::uint64_t pool_id,
                               std::size_t slot_count);

    ValueBufferResult Allocate();
    SourceInfo Identify(DataView value) const noexcept;

    void BeginClose() noexcept;
    void WaitUntilIdle() noexcept;

    DeviceId device_id() const noexcept;
    std::uint64_t pool_id() const noexcept;
    std::size_t slot_count() const noexcept;
};
```

说明：

- `Create` 在 `Open` 阶段调用；容量为 0 时成功返回空池。
- `Allocate` 不做 key 校验，也不做路由，只从当前设备池取槽位。
- `Identify` 是 P8M2 的内部来源识别基础，P8M2 不把它接入 `Put` 行为分支。
- `BeginClose` 只设置关闭状态并阻止新分配。
- `WaitUntilIdle` 等所有活跃 `ValueBuffer` 和正在分配的线程退出。

### 4.2 `ValueBuffer` 接入真实控制块

P8M2 修改 `ValueBuffer` 公开方法，新增显式释放：

```cpp
class ValueBuffer {
public:
    ValueBuffer() noexcept;
    ~ValueBuffer();

    ValueBuffer(ValueBuffer&& other) noexcept;
    ValueBuffer& operator=(ValueBuffer&& other) noexcept;

    ValueBuffer(const ValueBuffer&) = delete;
    ValueBuffer& operator=(const ValueBuffer&) = delete;

    DataBuffer data() noexcept;
    DataView view() const noexcept;
    bool valid() const noexcept;
    void reset() noexcept;
};
```

`reset()` 是 D7 严格关闭边界下的必要能力。否则调用方持有 `ValueBuffer` 时调用 `Close()`，只能等作用域
结束，容易在同一线程中自阻塞：

```cpp
auto r = engine.AllocateValueBuffer("k");
r.buffer.reset();
engine.Close(); // 不必等 r 离开作用域
```

`ValueBuffer` 控制块保存：

```text
pool_state
slot_index
slot_generation
data_address
released
```

`released` 必须是原子或等价的一次性释放保护，保证析构、`reset()`、移动赋值等路径不会重复归还同一槽位。

### 4.3 `Engine` 集成

`Engine` 新增每设备池引用：

```cpp
std::vector<std::shared_ptr<ValueBufferPool>> value_buffer_pools_;
```

`Open` 阶段：

```text
1. 校验 Options。
2. 逐设备 create / recover 到临时 DeviceContext。
3. 为每个设备创建 ValueBufferPool。
4. 将池引用放入对应 DeviceContext。
5. 全部设备成功后 move 进 Reactor 并 Start。
6. Engine 保存同序 value_buffer_pools_。
7. opened_ = true。
```

任一设备池创建失败时，`Open()` 必须走现有失败回滚路径：关闭已打开的设备句柄，释放已创建的池，返回错误。

`AllocateValueBuffer(key)`：

```text
if !opened_:
    kEngineNotOpen
if key.empty():
    kMemEmptyKey
if key.size() > kWalKeyMax:
    kWalKeyTooLong

device = RouteKey(key)
return value_buffer_pools_[device]->Allocate()
```

`Close()`：

```text
if !opened_:
    kEngineNotOpen

opened_ = false
for pool in value_buffer_pools_:
    pool->BeginClose()
for pool in value_buffer_pools_:
    pool->WaitUntilIdle()

for reactor in reactors_:
    reactor->Stop()
reactors_.clear()
value_buffer_pools_.clear()
```

`opened_ = false` 必须早于 `BeginClose()` 后的等待，避免新的公开分配继续进入当前打开周期。

### 4.4 `DeviceContext` / `reactor` 集成

`DeviceContext` 增加：

```cpp
std::shared_ptr<ValueBufferPool> value_buffer_pool;
```

P8M2 不要求 `reactor` 使用该池改变写入行为，但 `reactor` 必须能在后续 P8M3 通过 `dc_.value_buffer_pool`
识别 `DataView` 是否来自本设备池。

现有 `BufferPool pool{0}` 保持不变，继续服务 `ExecutePut` / `ExecuteGet` 的内部临时缓冲区。

---

## 5. 内存形态

### 5.1 slab 布局

每个设备池一块连续 slab：

```text
base: 1 MiB aligned
size: slot_count * 1 MiB

slot 0: [base + 0 MiB, base + 1 MiB)
slot 1: [base + 1 MiB, base + 2 MiB)
slot 2: [base + 2 MiB, base + 3 MiB)
...
```

只要 `base` 满足 1 MiB 对齐，每个槽位起点天然也是 1 MiB 对齐。

### 5.2 分配方式

P8M2 可使用标准 C 分配能力：

```cpp
void* p = std::aligned_alloc(kValueSize, slot_count * kValueSize);
```

约束：

- `slot_count * kValueSize` 必须做溢出检查；
- `std::aligned_alloc` 的 size 必须是 alignment 的整数倍，固定 1 MiB 槽位天然满足；
- 分配成功后用 `std::free` 释放；
- 容量为 0 时不分配 slab；
- 分配失败不能 `abort`，必须让 `Open()` 返回错误。

### 5.3 容量语义

```text
总槽位数 = devices.size() * value_buffer_pool_blocks
总内存占用 = 总槽位数 * 1 MiB
```

示例：

| 设备数 | `value_buffer_pool_blocks` | 总内存 |
|---:|---:|---:|
| 1 | 16 | 16 MiB |
| 2 | 16 | 32 MiB |
| 3 | 16 | 48 MiB |
| 4 | 64 | 256 MiB |

`value_buffer_pool_blocks = 0` 表示关闭公开分配能力，不影响普通 `Put`。

---

## 6. 无锁池设计

### 6.1 池共享状态

建议内部状态：

```text
PoolState
  device_id
  pool_id
  slot_count
  base_address
  total_size
  backend_kind
  backend_private

  closing: atomic<bool>
  active_count: atomic<uint32_t>
  free_head: atomic<uint64_t>
  slots[]
```

`active_count` 同时统计：

1. 已经成功分配、尚未释放的 `ValueBuffer`；
2. 已进入 `Allocate()`、但尚未完成成功或失败返回的分配线程。

这样 `Close()` 设置 `closing=true` 后，只要等待 `active_count==0`，即可知道没有未释放缓冲区，也没有正在穿越
分配临界区的调用线程。

### 6.2 槽位元数据

```text
Slot
  next_free: atomic<uint32_t>
  state: atomic<SlotState>
  generation: atomic<uint32_t>
  data_address
```

`SlotState` 至少包含：

```text
Free
Allocated
```

P8M2 不需要公开槽位状态。状态只用于：

- 防御重复释放；
- 来源识别；
- 测试断言；
- 未来后端注册信息关联。

### 6.3 带版本号空闲栈

空闲栈头使用 64 位原子：

```text
free_head = { tag: 32 bits, index: 32 bits }
index == UINT32_MAX 表示空栈
```

`tag` 每次 push / pop 成功后递增，避免 ABA：

```text
A 线程看到 head = slot 3, tag 10
B 线程 pop slot 3 -> push slot 3
head 又回到 slot 3，但 tag 已变
A 的 CAS 失败，不会把旧视图当成仍有效的栈头
```

槽位数量必须小于 `UINT32_MAX`；超出时视为非法配置，`Open()` 返回 `kEngineInvalidOpts`。

### 6.4 分配流程

```text
Allocate()
  if closing:
      return kEngineNotOpen

  active_count++
  if closing:
      active_count--
      notify_if_closing()
      return kEngineNotOpen

  loop:
      head = free_head.load(acquire)
      if head.index == invalid:
          active_count--
          notify_if_closing()
          return kEnginePoolExhausted

      next = slots[head.index].next_free.load(relaxed)
      desired = { head.tag + 1, next }
      if free_head.compare_exchange_weak(head, desired, acq_rel, acquire):
          slots[index].state = Allocated
          generation = slots[index].generation.load()
          return ValueBuffer(control_block(pool_state, index, generation, data_address))
```

说明：

- `active_count++` 必须发生在第二次 `closing` 检查前，避免 `Close()` 判断空闲后释放池，而分配线程继续进入。
- 池耗尽不等待，直接返回 `kEnginePoolExhausted`。
- 分配成功后，`active_count` 的一次计数由返回的 `ValueBuffer` 控制块持有，直到释放。

### 6.5 释放流程

```text
Release(slot)
  if control_block.released.exchange(true):
      return

  if slot generation 不匹配:
      drop_active_count_defensively()
      return

  slots[index].state = Free
  slots[index].generation++

  loop:
      head = free_head.load(acquire)
      slots[index].next_free = head.index
      desired = { head.tag + 1, index }
      if free_head.compare_exchange_weak(head, desired, release, acquire):
          break

  active_count--
  if closing:
      active_count.notify_all()
```

实现时要保证“释放一次”先于归还槽位，防止析构和 `reset()` 并发或移动赋值路径造成双重归还。

### 6.6 关闭等待

```text
BeginClose()
  closing.store(true, release)

WaitUntilIdle()
  while true:
      count = active_count.load(acquire)
      if count == 0:
          return
      active_count.wait(count, acquire)
```

释放路径在 `closing == true` 时应 `notify_all()`，并且建议每次 `active_count` 递减后都通知。这样 `Close()`
不会因为等待旧 count 而错过中间变化。

P8M2 的关闭等待是严格行为，不是性能路径。等待本身可以阻塞；分配和释放路径保持无锁。

### 6.7 内存序原则

| 操作 | 建议内存序 | 目的 |
|---|---|---|
| `closing.store(true)` | release | 发布关闭状态 |
| `closing.load()` | acquire | 分配线程观察关闭状态 |
| `active_count.fetch_add/sub` | acq_rel | 与 `Close()` 等待形成顺序 |
| `free_head` pop CAS | acq_rel / acquire | 获取槽位并观察空闲链内容 |
| `free_head` push CAS | release / acquire | 发布归还槽位 |
| `slot.state` 写入 | release | 发布槽位状态变化 |
| `slot.state` 读取 | acquire | 来源识别读取当前状态 |

不使用 `seq_cst`。P8M2 的无锁结构和 P7 的无锁路线保持一致：只在必要同步边上使用 release / acquire。

---

## 7. 来源识别

### 7.1 识别条件

`ValueBufferPool::Identify(DataView value)` 只在全部条件满足时返回匹配：

```text
value.size() == kValueSize
value.data() != nullptr
value.data() in [base, base + total_size)
(value.data() - base) % kValueSize == 0
slot_index < slot_count
slots[slot_index].state == Allocated
```

设备归属校验由调用者结合 key 路由结果完成：

```text
pool.device_id() == RouteKey(key)
```

P8M2 只实现和测试来源识别基础能力，不把识别结果接入 `Put` 的路径分支。

### 7.2 非匹配场景

| 场景 | 结果 |
|---|---|
| 普通 `std::vector<std::byte>` | 不匹配 |
| 普通 `aligned_alloc` 的 1 MiB 对齐内存 | 不匹配 |
| 地址来自槽位中间 | 不匹配 |
| 长度不是 1 MiB | 不匹配 |
| 地址属于另一个设备池 | 对当前池不匹配 |
| 槽位已释放且尚未重新分配 | 不匹配 |
| 池容量为 0 | 不匹配 |

普通外部分配内存即使地址和长度满足对齐，也不是 Cabe 值缓冲区。它后续可以在 P8M3/P8M4 进入应用自备内存
的条件式零拷贝判断，但不通过 `ValueBufferPool` 来源识别。

### 7.3 与后续零拷贝路径的关系

P8M3 / P8M4 会基于 P8M2 的识别结果做路径选择：

```text
来自目标设备 ValueBufferPool
  -> 主零拷贝候选

来自应用自备内存
  -> 条件式零拷贝候选

不满足条件
  -> 复制回退路径
```

P8M2 不承诺任何写入已经零拷贝。

---

## 8. 错误语义

P8M2 不新增错误码，复用既有错误码：

| 场景 | 返回 |
|---|---|
| `Engine` 未打开 | `kEngineNotOpen` |
| `Engine` 正在关闭或池已关闭 | `kEngineNotOpen` |
| key 为空 | `kMemEmptyKey` |
| key 过长 | `kWalKeyTooLong` |
| `value_buffer_pool_blocks = 0` | `kEnginePoolExhausted` |
| 池空闲槽位耗尽 | `kEnginePoolExhausted` |
| 打开阶段 slab 分配失败 | `kEnginePoolExhausted` |
| 配置导致容量计算溢出 | `kEngineInvalidOpts` |
| 槽位数量超出空闲栈可表达范围 | `kEngineInvalidOpts` |

打开阶段 slab 分配失败和运行期槽位耗尽都复用 `kEnginePoolExhausted`，但文档和日志应区分：

```text
Open 阶段：池资源无法建立
运行期：池槽位暂时耗尽
```

---

## 9. 生命周期与并发契约

### 9.1 推荐使用方式

```cpp
auto r = engine.AllocateValueBuffer(key);
if (!r.ok()) {
    return r.status;
}

auto buffer = std::move(r.buffer);
std::memcpy(buffer.data().data(), source, cabe::kValueSize);

auto st = engine.Put(key, buffer.view());
buffer.reset();
return st;
```

约束：

1. `ValueBuffer` 必须在分配它的 Engine 打开周期内释放。
2. `Put(key, buffer.view())` 调用期间，`ValueBuffer` 必须保持有效。
3. `Put` 是同步等待语义；`Put` 返回后可 `reset()` 或析构 `ValueBuffer`。
4. `ValueBuffer::data()` 返回的内存不自动清零。
5. 应用端不得在 `Put` 读取 `buffer.view()` 时并发修改 `buffer.data()`。
6. `ValueBuffer` 可移动到另一个线程，但同一个对象不支持多线程并发访问。

### 9.2 `Close()` 严格等待

正确示例：

```cpp
auto r = engine.AllocateValueBuffer("k");
r.buffer.reset();
engine.Close();
```

会阻塞的示例：

```cpp
auto r = engine.AllocateValueBuffer("k");
engine.Close(); // 等待 r.buffer reset 或析构
```

这不是死锁，而是严格关闭语义。如果调用方在同一线程中持有 `ValueBuffer` 并立即调用 `Close()`，应先调用
`reset()`。

### 9.3 与 P7 Open/Close 排他契约的关系

P8M2 不把 `Close()` 扩展为可与任意 Engine 方法并发。仍然遵循 P7M3 契约：

```text
Open / Close 是排他操作。
Put / Get / Delete / SetWalLevel / Snapshot 彼此可并发。
Close 前调用方应确保其他 Engine 方法已经返回。
```

P8M2 在此基础上增加：

```text
Close 还会等待所有已分配 ValueBuffer 释放。
```

---

## 10. 与既有组件的关系

### 10.1 与 `BufferPool`

`BufferPool` 仍是 reactor 私有的内部 I/O 临时缓冲池：

```text
Reactor::ExecutePut
  -> dc_.pool.Allocate()
  -> memcpy(value -> internal buffer)
  -> dc_.io.Write(...)
  -> dc_.pool.Free(...)
```

P8M2 不改变这条路径。

`ValueBufferPool` 是公开分配接口背后的 value 专用池：

```text
Engine::AllocateValueBuffer
  -> ValueBufferPool::Allocate
  -> ValueBuffer
```

两者不复用、不继承、不共享生命周期。

### 10.2 与 `reactor`

P8M2 中 `reactor` 只持有池引用，不使用池改变写入行为。这样后续 P8M3 可以在 `ExecutePut` 中直接访问
`dc_.value_buffer_pool` 做来源识别，不需要再改池归属。

### 10.3 与 `io_uring` / SPDK

P8M2 的普通 slab 内存不是 `io_uring` 注册缓冲区，也不是 SPDK 直接内存。P8M2 只建立：

- 固定 1 MiB value 内存；
- 1 MiB 地址对齐；
- 设备池归属；
- 后端私有字段预留；
- 关闭和释放边界。

P8M4 可以把 `io_uring` 注册缓冲区信息挂到池后端私有字段；P10 可以把底层 slab 分配替换为 SPDK 大页内存。

---

## 11. 实施顺序

推荐按以下顺序实现，便于每一步都有可测边界：

1. 新增 `ValueBufferPool` 文件和 CMake 条目。
2. 实现容量为 0 的空池和基础元数据。
3. 实现 slab 分配、1 MiB 对齐和析构释放。
4. 实现带版本号的无锁空闲栈初始化、pop、push。
5. 实现控制块释放逻辑，接入 `ValueBuffer::reset()` / 析构 / 移动赋值。
6. 实现 `BeginClose()` / `WaitUntilIdle()`。
7. 实现 `Identify(DataView)`。
8. 在 `Engine::Open` 中创建每设备池，并放入 `DeviceContext` 和 `Engine` 路由表。
9. 将 `Engine::AllocateValueBuffer` 从 P8M1 占位改为真实路由分配。
10. 将 `Engine::Close` 改为先关闭池并等待，再停 reactor 和清理资源。
11. 补齐池单测、Engine 集成、多线程和 TSAN 测试。

---

## 12. 测试计划

### 12.1 `ValueBufferPool` 单元测试

| 用例 | 内容 |
|---|---|
| `CreateZeroCapacity` | 容量为 0 时创建成功，无 slab，分配返回 `kEnginePoolExhausted`。 |
| `CreateAlignedSlab` | 容量 N 时每个返回地址均 1 MiB 对齐，`view().size()==kValueSize`。 |
| `Exhaustion` | 连续分配 N 个成功，第 N+1 个返回 `kEnginePoolExhausted`。 |
| `ReleaseAndReuse` | `reset()` 或析构后槽位可再次分配。 |
| `MoveReleasesOnce` | move 构造 / move 赋值后只有最终拥有者释放槽位。 |
| `ResetInvalidatesBuffer` | `reset()` 后 `valid()==false`，`data()` / `view()` 为空。 |
| `IdentifyAllocatedSlot` | 完整槽位起点 + 1 MiB 可识别。 |
| `IdentifyRejectsMiddleAddress` | 槽位中间地址不识别。 |
| `IdentifyRejectsWrongSize` | 长度不是 1 MiB 不识别。 |
| `IdentifyRejectsReleasedSlot` | 释放后且未重新分配的槽位不识别。 |
| `BeginCloseRejectsAllocate` | `BeginClose()` 后分配返回 `kEngineNotOpen`。 |
| `WaitUntilIdleBlocksUntilRelease` | 关闭等待在未释放 `ValueBuffer` 时不返回，释放后返回。 |

### 12.2 `Engine::AllocateValueBuffer` 集成测试

| 用例 | 内容 |
|---|---|
| `NotOpen` | 未打开时返回 `kEngineNotOpen`，buffer 无效。 |
| `EmptyKey` | 打开后空 key 返回 `kMemEmptyKey`。 |
| `KeyTooLong` | 打开后超长 key 返回 `kWalKeyTooLong`。 |
| `AllocatesValidBuffer` | 打开后合法 key 返回有效 `ValueBuffer`。 |
| `ZeroCapacity` | `value_buffer_pool_blocks=0` 时 Open 成功，分配返回 `kEnginePoolExhausted`。 |
| `ExhaustsPerDevicePool` | 同一设备容量耗尽后返回 `kEnginePoolExhausted`。 |
| `MultiDevicePoolsIndependent` | 多设备下不同路由设备的池容量独立。 |
| `CloseWaitsForOutstandingBuffer` | `Close()` 等待未释放 `ValueBuffer`，释放后返回。 |
| `AllocateAfterCloseFails` | `Close()` 后分配返回 `kEngineNotOpen`。 |

### 12.3 `ValueBuffer + Put/Get` 联动测试

P8M2 不测零拷贝，但要证明公开缓冲区可作为普通 value 输入：

```text
Open
AllocateValueBuffer(key)
填充 1 MiB
Put(key, buffer.view())
Get(key)
校验数据一致
Close
```

这条测试仍通过现有复制路径。

### 12.4 多线程与 TSAN

新增并发测试：

| 用例 | 内容 |
|---|---|
| `ConcurrentAllocateRelease` | 多线程反复分配、写入首尾标记、释放；无重复活跃槽位。 |
| `ConcurrentExhaustionBoundedByCapacity` | 竞争分配下成功持有数量不超过容量，失败为 `kEnginePoolExhausted`。 |
| `ConcurrentAllocateClose` | 一组线程分配释放，主线程进入 Close；Close 等待并最终返回。 |
| `ConcurrentMultiDeviceAllocateRelease` | 多设备下按 key 路由并发分配释放，设备池互不串扰。 |

TSAN 退出要求：

```text
sync 后端 + TSAN 下 P8M2 新增并发测试无 data race 报告。
```

`io_uring + TSAN` 继续按项目既有规则排除。

### 12.5 回归矩阵

P8M2 完成后建议至少跑：

```bash
./scripts/run-tests.sh --backend=sync --release
./scripts/run-tests.sh --backend=sync --asan
./scripts/run-tests.sh --backend=sync --ubsan
./scripts/run-tests.sh --backend=sync --tsan
./scripts/run-tests.sh --backend=io_uring --release
./scripts/run-tests.sh --backend=io_uring --asan
./scripts/run-tests.sh --backend=io_uring --ubsan
```

---

## 13. 退出条件

P8M2 完成时必须满足：

1. `Engine::AllocateValueBuffer(key)` 在合法 key 下能返回有效 `ValueBuffer`。
2. `ValueBuffer::data()` / `view()` 返回固定 1 MiB 视图。
3. 返回地址满足 1 MiB 对齐。
4. `value_buffer_pool_blocks` 真实生效。
5. 容量为 0 时 `Open()` 成功，分配返回 `kEnginePoolExhausted`。
6. 池耗尽返回 `kEnginePoolExhausted`，不阻塞等待。
7. slab 分配失败时 `Open()` 失败并回滚。
8. `ValueBuffer::reset()`、析构和 move 路径只释放一次。
9. `Close()` 等待所有未释放 `ValueBuffer` 后才返回。
10. `ValueBufferPool` 分配 / 释放核心路径无 mutex / condition_variable。
11. 来源识别能区分完整槽位、错误地址、错误长度、已释放槽位和外部内存。
12. 多设备池容量和归属互相独立。
13. `ValueBuffer.view()` 传给 `Put` 后仍可通过现有复制路径完成 Put/Get 往返。
14. sync 后端全量测试通过。
15. sync + TSAN 下 P8M2 并发测试无数据竞争。
16. `io_uring` 后端至少 release / ASAN / UBSAN 构建与测试通过。

---

## 14. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 无锁空闲栈 ABA | 同一槽位被错误重复分配或链表损坏 | 使用带版本号的 64 位栈头，CAS 同时比较 index + tag。 |
| `Close()` 自阻塞 | 调用方持有 `ValueBuffer` 时同线程调用 `Close()` 会等待 | 新增 `ValueBuffer::reset()`；文档强调关闭前释放。 |
| 关闭等待漏掉正在分配的线程 | `Close()` 过早释放池，分配线程 UAF | `active_count` 同时统计正在分配线程和已分配缓冲区。 |
| 来源识别误判外部内存 | 外部内存被错误当成 Cabe 值缓冲区 | 必须校验地址范围、槽位起点、长度、槽位状态和设备归属。 |
| 陈旧 `DataView` 别名新分配槽位 | `DataView` 无控制块身份，无法区分旧视图和新所有者 | 文档明确 `DataView` 不得超过 `ValueBuffer` 生命周期；P8 后续不把 stale view 定义为合法用法。 |
| 多设备归属错位 | key 路由到设备 A，却使用设备 B 的池 | `Engine` 和 `DeviceContext` 同序保存池；来源识别时校验 device_id。 |
| 内存占用过大 | 多设备大容量配置导致 Open 失败或系统压力 | 默认容量保守；容量可配置为 0；Open 阶段失败及时暴露。 |
| 过早绑定 `io_uring` / SPDK | 后续后端切换困难 | P8M2 只保留后端私有字段，不公开后端概念。 |
| P8M2 范围膨胀 | 无锁池和写入路径改动相互干扰 | P8M2 不改 `Put` 行为，零拷贝路径接入留给 P8M3/P8M4。 |

---

## 15. 后续衔接

P8M2 完成后，P8M3 可以在 `Reactor::ExecutePut` 中使用本里程碑提供的能力：

```text
dc_.value_buffer_pool->Identify(op->value)
  -> 判断是否来自当前设备池
  -> 判断是否满足主零拷贝候选条件
  -> 不满足则复制回退
```

P8M4 再把后端写入协议从裸指针升级为写入缓冲区描述符，并让 `io_uring` 后端使用注册缓冲区写入。

P8M2 的接口和元数据必须保证这两个后续里程碑不需要推翻 `ValueBuffer` 的公开语义。
