# Cabe P8-M1 设计：`ValueBuffer` 公开接口与术语落地

> 本里程碑固定 P8 零拷贝写入路径所需的公开 API：新增 `ValueBuffer`、`ValueBufferResult`、
> `Engine::AllocateValueBuffer(std::string_view key)`，并在 `Options` 中追加每设备值缓冲区池容量配置。
> M1 只落公开接口、对象语义、配置项、占位行为和测试边界；真实值缓冲区池、来源识别、路径选择和
> `io_uring` 注册缓冲区写入分别留给 P8M2 ~ P8M4。
>
> **本文为详细设计**，汇总 P8M1-D1 ~ P8M1-D11 的全部裁决。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P8 / M1 |
| 状态 | ✅ 已实装 |
| 上游依赖 | P1 ~ P7 已完成；P8 阶段总计划见 `doc/P8/README.md` |
| 下游依赖本里程碑 | P8M2 值缓冲区池抽象；P8M3 `Put` 路径选择；P8M4 后端写入协议；P8M5 bench 与收敛 |
| 退出判定 | 见 §10 |

---

## 1. 目标与范围

### 1.1 目标

1. **公开 `ValueBuffer` 类型**：应用端可以从 Cabe 分配固定 1 MiB 的 Cabe 值缓冲区，并通过
   `DataBuffer` 填充。
2. **公开分配接口**：在 `Engine` 上新增
   `ValueBufferResult AllocateValueBuffer(std::string_view key)`。
3. **保持 `Put` 统一入口**：应用端填充 `ValueBuffer` 后仍调用
   `Engine::Put(key, buffer.view())`；不新增 `Put(ValueBuffer&&)` 或“强制零拷贝”接口。
4. **固定对象语义**：`ValueBuffer` 是移动专属 RAII 对象；禁止拷贝；析构自动归还内部资源。
5. **固定配置项**：在 `Options` 末尾追加 `value_buffer_pool_blocks`，表示每设备 Cabe 值缓冲区数量。
6. **固定 M1 占位行为**：M1 通过分配接口前置校验后，暂时返回 `kEngineNotImplemented` 和无效
   `ValueBuffer`。
7. **固定测试边界**：M1 只验证公开 API、类型语义、配置默认值和前置校验；不验证真实零拷贝路径。

### 1.2 交付范围

1. **`engine/value_buffer.h`**（新建）：
   `ValueBuffer`、`ValueBufferResult` 及内部控制块前向声明。
2. **`engine/value_buffer.cpp`**（新建）：
   `ValueBuffer` 默认构造、析构、移动、`data()`、`view()`、`valid()`、`ValueBufferResult::ok()`。
3. **`engine/engine.h`**（修改）：
   引入 `value_buffer.h`，声明 `AllocateValueBuffer(std::string_view key)`。
4. **`engine/engine.cpp`**（修改）：
   实现 M1 占位分配逻辑：未打开、空 key、key 过长按既有错误码返回；合法 key 暂返
   `kEngineNotImplemented`。
5. **`engine/options.h`**（修改）：
   在 `Options` 末尾追加 `std::size_t value_buffer_pool_blocks = 16`。
6. **`engine/CMakeLists.txt`**（修改）：
   将 `value_buffer.cpp` 加入 `cabe_engine`。
7. **`test/engine/value_buffer_test.cpp`**（新建）：
   覆盖类型语义、无效对象行为、结果类型、配置默认值和分配接口前置校验。
8. **`test/CMakeLists.txt`**（修改）：
   注册 `test_value_buffer`。
9. **`CONTEXT.md` / `doc/P8/*`**（已更新 / 本文）：
   固定 P8M1 相关术语和设计结论。

### 1.3 明确不做

| 不做项 | 归属 |
|---|---|
| 真实 `ValueBufferPool` | P8M2 |
| 每设备 1 MiB 对齐内存池 | P8M2 |
| 值内存来源识别 | P8M2 / P8M3 |
| `ValueBuffer` 设备归属校验 | P8M2 / P8M3 |
| `Reactor::ExecutePut` 零拷贝路径选择 | P8M3 |
| `IoBackend::Write` 写入描述符升级 | P8M4 |
| `io_uring` 注册缓冲区写入 | P8M4 |
| SPDK 后端或 SPDK 大页内存接入 | P10 |
| bench | P8M5 |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 |
|---|---|---|
| **P8M1-D1** | `ValueBuffer` 公开边界 | 新增公开 `ValueBuffer`，放入 `engine/value_buffer.h`；只暴露填充、视图、有效性和移动语义，不暴露后端信息。 |
| **P8M1-D2** | 分配接口形态 | 新增 `ValueBufferResult Engine::AllocateValueBuffer(std::string_view key)`；写入仍统一走 `Put`。 |
| **P8M1-D3** | 分配结果与错误表达 | 使用 `{ Status status; ValueBuffer buffer; bool ok() const; }`；成功时 buffer 有效，失败时 buffer 无效。 |
| **P8M1-D4** | `ValueBuffer` 对象语义 | 移动专属 RAII 对象；默认无效；析构自动归还；禁止拷贝。M1 暂不提供手动释放；P8M2 因严格关闭边界补充 `reset()`。 |
| **P8M1-D5** | 填充与提交方式 | 只提供 `data()` 和 `view()`；应用必须填满 1 MiB；提交仍调用 `Engine::Put(key, buffer.view())`；不承诺自动清零。 |
| **P8M1-D6** | key 与设备归属 | 分配接口必须接收 key；P8M1 先固定 key 用于计算目标设备归属；P8M3 已将最终性能路径语义收紧为键绑定。 |
| **P8M1-D7** | 分配接口校验语义 | 复用 `Put` 的 key 校验：未打开、空 key、key 过长分别返回既有错误码；池耗尽用 `kEnginePoolExhausted`。 |
| **P8M1-D8** | 容量配置 | 在 `Options` 末尾追加 `std::size_t value_buffer_pool_blocks = 16`；含义为每设备值缓冲区数量；允许为 0。 |
| **P8M1-D9** | `Close` 与生命周期 | M1 无真实池，仅固定公开对象形态；P8M2 已将最终语义收紧为严格打开周期：`Close()` 等待所有已分配 `ValueBuffer` 释放，不允许 `ValueBuffer` 跨 `Close/Open` 存活。 |
| **P8M1-D10** | M1 实现边界 | M1 只实现 API、对象语义、配置项和占位分配行为；合法 key 通过前置校验后暂返 `kEngineNotImplemented`。 |
| **P8M1-D11** | 测试边界 | 新增 `test_value_buffer`；只测类型语义、默认配置和分配接口前置校验；真实分配、对齐、路径选择和 bench 后移。 |

---

## 3. 术语与模型

### 3.1 值内存与 Cabe 值缓冲区

P8 将 value 来源分成两类：

| 来源 | 含义 | P8M1 处理 |
|---|---|---|
| 应用端自备值内存 | 应用端自行分配，并以 `DataView` 传给 `Put` 的 1 MiB 内存 | M1 不改变现有 `Put` 行为 |
| Cabe 值缓冲区 | Cabe 分配给应用端填充 value 的固定 1 MiB 资源对象 | M1 固定公开 `ValueBuffer` 语义 |

`ValueBuffer` 不是普通动态数组，也不是写入操作本身。它只是 Cabe 分配出来、便于后续主零拷贝路径识别和使用的 value 内存容器。

### 3.2 Engine 打开周期

**Engine 打开周期**指一次 `Engine::Open` 成功到对应 `Engine::Close` 完成之间的时间段。

P8M1 只固定该术语；P8M2 已将最终语义收紧为严格打开周期：`ValueBuffer` 必须在分配它的 Engine 打开周期内释放，`Engine::Close()` 等待所有已分配 `ValueBuffer` 释放后才返回。

### 3.3 key 的含义与后续键绑定

P8M1 中，`AllocateValueBuffer(key)` 先固定 key 用于计算目标设备归属：

```text
key -> RouteKey(key) -> 目标设备 -> 对应设备的值缓冲区池
```

P8M3 已将最终性能路径语义收紧为键绑定：`AllocateValueBuffer(key_a)` 返回的 `ValueBuffer` 只有在
`Put(key_a, buffer.view())` 时才可以命中主直接写入路径；`Put(key_b, buffer.view())` 即使路由到同一设备，
也必须复制回退。键不匹配不是公开写入错误，只影响是否进入主直接写入路径。

P8M1 不实现该路径判断，只固定公开接口形态。

---

## 4. 公开 API 设计

### 4.1 `engine/value_buffer.h`

建议头文件形态：

```cpp
#ifndef CABE_VALUE_BUFFER_H
#define CABE_VALUE_BUFFER_H

#include "common/structs.h"
#include "engine/status.h"

#include <memory>

namespace cabe {

namespace detail {
    struct ValueBufferControlBlock;
}

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

private:
    friend class Engine;
    friend class ValueBufferPool;       // P8M2 引入

    ValueBuffer(std::shared_ptr<detail::ValueBufferControlBlock> control,
                std::byte* data) noexcept;

    std::shared_ptr<detail::ValueBufferControlBlock> control_;
    std::byte* data_ = nullptr;
};

struct ValueBufferResult {
    Status status;
    ValueBuffer buffer;

    bool ok() const noexcept;
};

} // namespace cabe

#endif // CABE_VALUE_BUFFER_H
```

说明：

- `ValueBufferControlBlock` 是内部生命周期控制块，不进入公开语义。
- `std::shared_ptr` 是 M1 的控制块占位实现；P8M2 会将其接入真实池释放和严格 `Close()` 等待语义。
- `ValueBuffer` 禁止拷贝，即使内部使用共享控制块，也不允许应用端共享同一个资源所有权。
- `ValueBufferPool` 在 P8M2 引入；M1 只预留友元声明即可。

### 4.2 `ValueBuffer` 方法语义

| 方法 | 语义 |
|---|---|
| `ValueBuffer()` | 构造无效对象，不持有资源。 |
| `~ValueBuffer()` | 若持有资源，自动归还；无效对象析构无操作。 |
| 移动构造 | 接管资源，源对象变为无效。 |
| 移动赋值 | 先归还当前资源，再接管新资源；源对象变为无效。 |
| `data()` | 有效对象返回 1 MiB 可写 `DataBuffer`；无效对象返回空视图。 |
| `view()` | 有效对象返回 1 MiB 只读 `DataView`；无效对象返回空视图。 |
| `valid()` | 仅表示当前对象是否持有 Cabe 值缓冲区资源。 |

P8M1 的 `ValueBuffer` 不提供：

- `release()`；
- `Free()`；
- `resize()`；
- `size()`；
- `Put()`；
- `Commit()`；
- `As<T>()`；
- `CopyFrom()`。

原因：`ValueBuffer` 不是通用容器，也不是写入入口。它只负责承载 Cabe 值缓冲区的所有权和视图访问。
P8M2 因严格 `Close()` 等待语义，会在此基础上补充 `reset()`，用于调用方在作用域结束前显式归还槽位。

### 4.3 `ValueBufferResult`

分配结果结构：

```cpp
struct ValueBufferResult {
    Status status;
    ValueBuffer buffer;

    bool ok() const noexcept { return status.ok(); }
};
```

语义：

| 情况 | `status` | `buffer` |
|---|---|---|
| 分配成功 | `Status::Ok()` | 有效 `ValueBuffer` |
| 分配失败 | 具体错误码 | 无效 `ValueBuffer` |

`ValueBuffer::valid()` 不表达失败原因；错误语义只来自 `Status`。

### 4.4 `Engine` 新增方法

`engine/engine.h` 新增：

```cpp
ValueBufferResult AllocateValueBuffer(std::string_view key);
```

调用示例：

```cpp
auto r = engine.AllocateValueBuffer(key);
if (!r.ok()) {
    return r.status;
}

auto buffer = std::move(r.buffer);
std::memcpy(buffer.data().data(), source, cabe::kValueSize);

return engine.Put(key, buffer.view());
```

约束：

- `AllocateValueBuffer` 只准备 Cabe 值缓冲区，不执行写入。
- `Put` 仍然是唯一公开写入入口。
- 应用端必须在 `Put` 前写满整个 1 MiB。
- 分配不承诺自动清零。
- `Put` 执行期间不得并发修改该 `ValueBuffer` 的内容。

### 4.5 `Options` 新增配置

`engine/options.h` 在 `Options` 末尾追加：

```cpp
std::size_t value_buffer_pool_blocks = 16;
```

语义：

- 每个数据设备对应的 Cabe 值缓冲区池容量；
- 每个块固定 1 MiB；
- `devices.size() * value_buffer_pool_blocks * kValueSize` 是值缓冲区池的理论内存规模；
- 允许配置为 0；
- 配置为 0 时普通 `Put` 不受影响，`AllocateValueBuffer` 在真实池实现后返回 `kEnginePoolExhausted`；
- M1 暂不使用该字段，只固定公开配置语义。

示例：

| 设备数 | `value_buffer_pool_blocks` | 理论池内存 |
|---|---:|---:|
| 1 | 16 | 16 MiB |
| 2 | 16 | 32 MiB |
| 4 | 64 | 256 MiB |

---

## 5. M1 占位实现

### 5.1 `ValueBuffer` 无效对象

M1 没有真实池，因此不会构造有效 `ValueBuffer`。默认对象行为必须完整：

```cpp
ValueBuffer::ValueBuffer() noexcept = default;

bool ValueBuffer::valid() const noexcept {
    return data_ != nullptr && control_ != nullptr;
}

DataBuffer ValueBuffer::data() noexcept {
    if (!valid()) return {};
    return DataBuffer{data_, kValueSize};
}

DataView ValueBuffer::view() const noexcept {
    if (!valid()) return {};
    return DataView{data_, kValueSize};
}
```

移动语义：

- 移动后源对象必须变为无效；
- 移动赋值接管前应先释放当前资源；
- M1 当前没有有效资源，但实现必须按最终语义写，避免 M2 再改公开对象行为。

### 5.2 `AllocateValueBuffer` 占位逻辑

`engine/engine.cpp` 新增：

```cpp
ValueBufferResult Engine::AllocateValueBuffer(std::string_view key) {
    if (!opened_.load(std::memory_order_acquire)) {
        return {Status::Error(err::kEngineNotOpen), {}};
    }
    if (key.empty()) {
        return {Status::Error(err::kMemEmptyKey), {}};
    }
    if (key.size() > kWalKeyMax) {
        return {Status::Error(err::kWalKeyTooLong), {}};
    }

    return {Status::Error(err::kEngineNotImplemented), {}};
}
```

说明：

- 前置校验与 `Put` 的 key 校验保持一致。
- M1 不路由、不分配、不投递 `reactor`。
- `kEngineNotImplemented` 是 M1 临时状态；P8M2 完成真实池后应退场。

### 5.3 错误码使用

| 场景 | M1 返回 | P8 最终返回 |
|---|---|---|
| Engine 未打开 | `kEngineNotOpen` | 同 M1 |
| key 为空 | `kMemEmptyKey` | 同 M1 |
| key 过长 | `kWalKeyTooLong` | 同 M1 |
| 合法 key，但 M1 未实现真实池 | `kEngineNotImplemented` | 不应保留 |
| 值缓冲区池耗尽 | M1 不触发 | `kEnginePoolExhausted` |

M1 不新增错误码。

---

## 6. 生命周期与并发语义

### 6.1 生命周期规则

`ValueBuffer` 推荐使用方式：

```text
Engine::Open
  -> AllocateValueBuffer(key)
  -> 应用端填充 buffer.data()
  -> Engine::Put(key, buffer.view())
  -> ValueBuffer 析构或移动销毁
Engine::Close
```

约束：

1. `ValueBuffer` 必须在分配它的 Engine 打开周期内使用和释放。
2. `Put(key, buffer.view())` 调用期间，`ValueBuffer` 必须保持存活。
3. 当前 `Put` 是同步等待语义，因此 `Put` 返回后即可释放或复用该 `ValueBuffer`。
4. P8M1 尚无真实池，不触发关闭等待；P8M2 起，`Engine::Close` 必须等待所有 `ValueBuffer` 释放。
5. 跨 `Close/Open` 使用旧 `ValueBuffer` 不属于合法行为。

### 6.2 并发规则

`ValueBuffer` 自身不承诺线程安全：

- 可以把 `ValueBuffer` 移动到另一个线程；
- 不允许多个线程同时写同一个 `ValueBuffer`；
- `Put` 正在读取 `buffer.view()` 时，应用端不得修改 `buffer.data()`；
- 多个不同 `ValueBuffer` 可由不同线程独立填充和提交；
- 真实资源池的并发分配策略留给 P8M2。

### 6.3 跨 `Close/Open` 场景

P8M2 已将最终语义收紧为严格打开周期。下面这种场景不属于合法用法：

```cpp
cabe::ValueBuffer old;

{
    cabe::Engine engine;
    engine.Open(opts);
    auto r = engine.AllocateValueBuffer("a");
    old = std::move(r.buffer);
    engine.Close();
}

// old 晚于 Close 才释放：P8M2 起 Close 会等待，调用方应先 reset 或让 old 离开作用域
```

设计要求：

- `Engine::Close()` 进入关闭流程后拒绝新的公开分配；
- `Engine::Close()` 必须等待所有已分配 `ValueBuffer` 释放；
- `Close()` 返回后不允许存在旧周期 `ValueBuffer`；
- 不能把旧打开周期的内部槽位误识别为新打开周期的 Cabe 值缓冲区。

M2 实现时应使用内部控制块、无锁计数和 `reset()` 等机制满足这些要求。

---

## 7. 与后续里程碑的关系

### 7.1 P8M2：值缓冲区池

P8M2 将在 M1 API 基础上补齐：

- `ValueBufferPool` 内部抽象；
- 每设备池；
- 1 MiB 对齐；
- 分配、释放、来源识别；
- 设备归属；
- `value_buffer_pool_blocks` 真实生效；
- 池耗尽返回 `kEnginePoolExhausted`。

P8M1 不能把任何 `io_uring` 或 SPDK 专属信息放进公开 `ValueBuffer`。

### 7.2 P8M3：`Put` 路径选择

P8M3 将在 `Reactor::ExecutePut` 中处理：

- 判断 `DataView` 是否来自当前打开周期的 Cabe 值缓冲区；
- 校验设备归属是否匹配实际写入 key；
- 匹配时走主零拷贝路径；
- 不匹配时复制回退；
- 应用端自备内存满足条件时进入条件式零拷贝路径。

P8M1 不改当前 `Put` 行为。

### 7.3 P8M4：后端写入协议

P8M4 将升级：

- 内部写入缓冲区描述符；
- `IoBackend` 写入接口；
- `io_uring` 注册缓冲区；
- 注册缓冲区槽位和后端私有信息传递。

P8M1 的 `ValueBuffer` 不暴露这些后端细节。

### 7.4 P10：SPDK

P8M1 的 API 必须支持未来 SPDK：

- `ValueBuffer` 可以映射到 SPDK 大页内存；
- 上层应用不需要知道 SPDK 分配细节；
- 公开 `Put` 入口不变；
- 公开 API 不暴露 `io_uring` 注册缓冲区概念。

---

## 8. 测试计划

### 8.1 新增测试目标

新增：

```cmake
add_executable(test_value_buffer engine/value_buffer_test.cpp)
target_link_libraries(test_value_buffer PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_value_buffer DISCOVERY_TIMEOUT 60)
```

### 8.2 测试用例

| 用例 | 内容 |
|---|---|
| `ValueBuffer.DefaultInvalid` | 默认构造后 `valid()==false`，`data().empty()`，`view().empty()`。 |
| `ValueBuffer.TypeTraits` | `static_assert` 不可拷贝、可移动。 |
| `ValueBufferResult.OkDelegatesToStatus` | `ok()` 与 `status.ok()` 一致。 |
| `Options.DefaultValueBufferPoolBlocks` | `Options{}.value_buffer_pool_blocks == 16`。 |
| `EngineAllocateValueBuffer.NotOpen` | 未打开 Engine 时返回 `kEngineNotOpen`，buffer 无效。 |
| `EngineAllocateValueBuffer.EmptyKey` | 打开后空 key 返回 `kMemEmptyKey`，buffer 无效。 |
| `EngineAllocateValueBuffer.KeyTooLong` | 打开后超长 key 返回 `kWalKeyTooLong`，buffer 无效。 |
| `EngineAllocateValueBuffer.M1Placeholder` | 打开后合法 key 返回 `kEngineNotImplemented`，buffer 无效。 |

打开 Engine 的测试沿用现有设备环境变量：

- `CABE_TEST_DEVICE`
- `CABE_TEST_WAL_DEVICE`
- `CABE_TEST_SNAPSHOT_DEVICE`

环境变量缺失时跳过需要设备的用例。

### 8.3 不在 M1 覆盖

| 不覆盖项 | 原因 |
|---|---|
| 分配成功返回有效 `ValueBuffer` | M1 不实现真实池 |
| 1 MiB 对齐 | P8M2 职责 |
| 池耗尽 | P8M2 职责 |
| `Put` 命中主零拷贝路径 | P8M3 职责 |
| key A 分配、key B 写入路径选择 | P8M3 职责 |
| `io_uring` 注册缓冲区写入 | P8M4 职责 |
| bench | P8M5 职责 |

### 8.4 签收命令

最小签收：

```bash
cmake --build build
ctest --test-dir build --output-on-failure -R 'test_value_buffer|test_engine'
```

阶段合入前建议按项目既有矩阵跑全量回归：

```bash
./scripts/run-tests.sh --backend=sync --release
./scripts/run-tests.sh --backend=sync --asan
./scripts/run-tests.sh --backend=sync --ubsan
./scripts/run-tests.sh --backend=sync --tsan
./scripts/run-tests.sh --backend=io_uring --release
./scripts/run-tests.sh --backend=io_uring --asan
./scripts/run-tests.sh --backend=io_uring --ubsan
```

`io_uring + TSAN` 继续按既有规则排除。

---

## 9. 风险与缓解

| 风险 | 影响 | 缓解 |
|---|---|---|
| 公开 API 设计过宽 | 后续 SPDK 接入被 API 绑死 | `ValueBuffer` 不暴露设备、槽位、注册缓冲区和后端私有信息。 |
| M1 临时实现被误认为可用零拷贝 | 用户调用后困惑 | 合法 key 暂返 `kEngineNotImplemented`；文档明确 M2 才实现真实分配。 |
| `ValueBuffer` 跨 `Close` 造成关闭边界不清 | 资源泄漏或下一次 Open 被旧资源污染 | P8M2 采用严格关闭边界：`Close()` 等待所有 `ValueBuffer` 释放。 |
| 默认池容量增加内存占用 | 多设备场景常驻内存增加 | 默认 16 块每设备，允许配置为 0；部署文档后续说明资源规模。 |
| `ValueBuffer` 身份通过 `DataView` 传递会丢失显式类型 | 后续路径识别需要内部登记 | M2 负责来源识别；M1 不新增 `Put` 重载，保持公开写入接口统一。 |
| 用户只填一部分 value | 写入未初始化业务数据 | 文档明确应用端必须写满 1 MiB；Cabe 只校验大小。 |
| 旧打开周期槽位被误识别 | 错用旧池槽位或旧后端私有信息 | P8M2 采用严格 `Close()` 等待和当前池身份校验，避免旧周期资源进入新周期。 |
| M1 不测有效对象移动 | 有效资源移动 bug 留到 M2 | M1 无真实有效对象；M2 在真实池测试中补充有效对象移动和释放。 |

---

## 10. 退出条件

P8M1 完成时必须满足：

1. `engine/value_buffer.h` 和 `engine/value_buffer.cpp` 已新增。
2. `ValueBuffer` 公开 API 与本文一致。
3. `ValueBufferResult` 公开 API 与本文一致。
4. `Engine::AllocateValueBuffer(std::string_view key)` 已声明并有 M1 占位实现。
5. `Options::value_buffer_pool_blocks` 已追加，默认值为 16。
6. `Put` 公开签名不变。
7. `Reactor::ExecutePut` 行为不变。
8. `IoBackend` 行为不变。
9. `test_value_buffer` 已新增并通过。
10. 现有 `test_engine` 不退步。
11. 文档明确 M1 不实现真实分配和零拷贝路径。

---

## 11. 后续工作入口

P8M1 完成后进入 P8M2，重点讨论：

1. `ValueBufferPool` 抽象层边界；
2. 每设备池的所有权放置位置；
3. 1 MiB 对齐分配方式；
4. `ValueBuffer` 内部控制块如何归还资源；
5. `DataView` 来源识别结构；
6. 池耗尽和 `Close` 时未归还缓冲区的处理；
7. 为 P8M3 / P8M4 保留的设备归属和后端私有字段。
