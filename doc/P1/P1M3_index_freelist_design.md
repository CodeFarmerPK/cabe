# Cabe P1-M3 设计：索引与空间管理（FreeList + MetaIndex + RouteKey）

> 本里程碑实装 `FreeList`（`vector<BlockId>` + LIFO 空间分配）和 `MetaIndex`
> （`unordered_map<string, ValueMeta>` 索引），补入 `DeviceContext`，并在 `Engine` 上
> 新增 `RouteKey`（P1 返回 0 占位）。内部组件方法返回 `int32_t` 错误码（按返回值分层约定）。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P1 / M3 |
| 状态 | **✅ 已锁定（P1M5 收敛）**
| 上游依赖 | P1M1（Engine 骨架 + Status / Options）、P1M2（BufferPool + I/O + O_DIRECT） |
| 下游依赖本里程碑 | P1M4（Put / Get / Delete 端到端打通——串联 FreeList + MetaIndex + BufferPool + I/O） |
| 关联约束 | ROADMAP P1 范围；返回值分层约定（公开 API 用 Status，内部用 int32_t） |
| 退出判定 | 见 §10（六条） |

---

## 1. 目标与范围

### 1.1 目标

1. 实装 `FreeList`（`vector<BlockId>` + LIFO）：Open 时从设备大小自动计算 `block_count`，
   预填 `[0, block_count)` 所有 BlockId；提供 `Allocate(BlockId* out)` / `Free(BlockId)`。
2. 实装 `MetaIndex`（`unordered_map<string, ValueMeta>`）：提供 `Insert` / `Lookup` / `Delete` / `Size`。
3. `DeviceContext` 补入 `FreeList` + `MetaIndex` 字段。
4. `Engine` 新增 `RouteKey` 方法（P1 返回 0 占位）。
5. `Engine::Open` 补入 FreeList 初始化（从设备大小算 block_count）。
6. 扩展 `common/error_code.h`：engine 段加 `kEngineNoSpace`，index 段加 `kIndexKeyNotFound`。
7. 编写 FreeList + MetaIndex 单元测试。

### 1.2 交付范围（本里程碑产出）

1. **`engine/free_list.h` + `engine/free_list.cpp`**：FreeList 类。
2. **`engine/meta_index.h` + `engine/meta_index.cpp`**：MetaIndex 类。
3. **`engine/device_context.h` 修改**：补入 `FreeList` + `MetaIndex` 字段。
4. **`engine/engine.h` 修改**：新增 `RouteKey` 私有方法。
5. **`engine/engine.cpp` 修改**：`Open` 补入设备大小查询 + FreeList 初始化；`RouteKey` 实现。
6. **`common/error_code.h` 修改**：新增 2 个错误码。
7. **`engine/CMakeLists.txt` 修改**：加 `free_list.cpp` / `meta_index.cpp`。
8. **`test/engine/free_list_test.cpp`**：FreeList 单元测试。
9. **`test/engine/meta_index_test.cpp`**：MetaIndex 单元测试。
10. **`test/CMakeLists.txt` 修改**：注册新测试。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| Put / Get / Delete 完整路径 | **P1M4** | 本里程碑只做组件，端到端串联在 M4 |
| MetaIndex 抽象层（C++20 concept） | **P3** | P1 不做抽象——直接 unordered_map |
| MetaIndex 透明查找优化 | **P3+** | P1 用 `std::string(key)` 显式构造查找，性能不是瓶颈 |
| FreeList 三容器轮换 | **P4.5** | P1 朴素 LIFO 单栈 |

---

## 2. 现状盘点

- **P1M2 已落地**：`DeviceContext { int fd; BufferPool pool; }`——需补入 FreeList + MetaIndex。
- **Engine::Open** 用 `O_DIRECT` 打开设备 + 初始化 BufferPool——需补入设备大小查询 + FreeList 初始化。
- **Engine 无 RouteKey**——P1M3 新增。
- **I/O 层已就位**：`WriteBlock` / `ReadBlock` 返回 `int32_t`——FreeList 分配的 BlockId 的
  `block_idx()` 直接传给 I/O 层（P1M4 串联时用到）。
- **错误码段位**：engine 段已有 5 个码（`kEngineAlreadyOpen` ~ `kEngineNotImplemented`）；
  index 段（`kIndexBase = -102000`）尚未分配任何码。

---

## 3. 关键决策（owner 已拍板）

| 编号 | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| **P1M3-D1** | MetaIndex key 用 `std::string` 拥有副本 | `string_view`（调用方持有 buffer） | 安全、简单；P1 单线程不关心 string 拷贝开销 | 锁定 |
| **P1M3-D2** | RouteKey 返回 `return 0;` | `Hash(key) % 1`（恒等 0 但多一次 hash） | 诚实表达"P1 不路由"；P3 改一行 | 锁定 |
| **P1M3-D3** | FreeList 从设备大小自动算 `block_count`（尾部不足 1 MiB 弃用）；封装成类，方法返回 `int32_t` | Options 传 block_count / 硬编码 / 裸 vector | 自动适配设备大小；封装与 BufferPool 风格一致 | 锁定 |
| **P1M3-D4** | MetaIndex 封装成类，`Insert` / `Lookup(key, ValueMeta* out)` / `Delete` 返回 `int32_t` | 裸 unordered_map | 与 FreeList 风格对称；P3 改造友好 | 锁定 |

---

## 4. FreeList 设计

### 4.1 类声明

```cpp
// engine/free_list.h
#ifndef CABE_FREE_LIST_H
#define CABE_FREE_LIST_H

#include "common/structs.h"
#include "common/error_code.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace cabe {

    class FreeList {
    public:
        FreeList() = default;

        // 初始化：预填 [0, block_count) 的 BlockId（device_id 绑定 dev）。
        // 尾部不足 kValueSize 的空间被丢弃（P5：block_count = (device_bytes - kDataRegionOffset) / kValueSize，头部 8K 为超级块）。
        int32_t Init(DeviceId dev, std::uint64_t block_count);

        // 分配一个空闲块。成功写 *out 返回 kSuccess；空返回 kEngineNoSpace。
        int32_t Allocate(BlockId* out);

        // 归还一个块（Delete 释放块号时调）。
        void Free(BlockId id);

        std::size_t available() const noexcept;
        bool empty() const noexcept;

    private:
        std::vector<BlockId> stack_;
    };

} // namespace cabe

#endif // CABE_FREE_LIST_H
```

### 4.2 实现要点

**Init**：
```cpp
int32_t FreeList::Init(DeviceId dev, std::uint64_t block_count) {
    stack_.clear();
    stack_.reserve(block_count);
    // 倒序压入：pop_back 时按地址顺序分配（block_idx 0 → 1 → 2 → ...）
    for (std::uint64_t i = block_count; i > 0; --i) {
        stack_.push_back(BlockId::Make(dev, i - 1));
    }
    return err::kSuccess;
}
```

- 倒序压入使 `pop_back` 时先分配低地址块——对顺序写友好。
- `block_count` 由调用方（Engine::Open）从设备大小算出传入。

**Allocate**：
```cpp
int32_t FreeList::Allocate(BlockId* out) {
    if (stack_.empty()) return err::kEngineNoSpace;
    *out = stack_.back();
    stack_.pop_back();
    return err::kSuccess;
}
```

**Free**：
```cpp
void FreeList::Free(BlockId id) {
    stack_.push_back(id);
}
```

- 不做重复归还校验（P1 信任内部代码；P4.5 FreeList 改造时加）。

**Move 语义**：`vector` 默认支持 move——FreeList 不需要自定义。

---

## 5. MetaIndex 设计

### 5.1 类声明

```cpp
// engine/meta_index.h
#ifndef CABE_META_INDEX_H
#define CABE_META_INDEX_H

#include "common/structs.h"
#include "common/error_code.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cabe {

    class MetaIndex {
    public:
        MetaIndex() = default;

        // 插入或覆盖（同 key 重复 Put 覆盖旧 meta）。
        int32_t Insert(std::string_view key, const ValueMeta& meta);

        // 查询。找到写 *out 返回 kSuccess；未找到返回 kIndexKeyNotFound。
        int32_t Lookup(std::string_view key, ValueMeta* out) const;

        // 删除。找到删除返回 kSuccess；未找到返回 kIndexKeyNotFound。
        int32_t Delete(std::string_view key);

        std::size_t Size() const noexcept;
        bool Contains(std::string_view key) const;

    private:
        std::unordered_map<std::string, ValueMeta> map_;
    };

} // namespace cabe

#endif // CABE_META_INDEX_H
```

### 5.2 实现要点

**Insert**：
```cpp
int32_t MetaIndex::Insert(std::string_view key, const ValueMeta& meta) {
    map_[std::string(key)] = meta;
    return err::kSuccess;
}
```

- `map_[string(key)]`：key 不存在时插入 + 赋值；key 存在时覆盖。
- 覆盖语义：cabe 的 Put 是"最后一次写生效"——重复 key 覆盖旧 meta（含旧 BlockId——
  Engine 层负责先 FreeList.Free 旧块再 Insert 新 meta，详见 P1M4）。

**Lookup**：
```cpp
int32_t MetaIndex::Lookup(std::string_view key, ValueMeta* out) const {
    auto it = map_.find(std::string(key));
    if (it == map_.end()) return err::kIndexKeyNotFound;
    *out = it->second;
    return err::kSuccess;
}
```

- `std::string(key)`：P1 显式构造临时 string 做查找；P3 可优化为透明查找。

**Delete**：
```cpp
int32_t MetaIndex::Delete(std::string_view key) {
    auto it = map_.find(std::string(key));
    if (it == map_.end()) return err::kIndexKeyNotFound;
    map_.erase(it);
    return err::kSuccess;
}
```

**Size / Contains**：
```cpp
std::size_t MetaIndex::Size() const noexcept { return map_.size(); }
bool MetaIndex::Contains(std::string_view key) const {
    return map_.count(std::string(key)) > 0;
}
```

**Move 语义**：`unordered_map` 默认支持 move——MetaIndex 不需要自定义。

---

## 6. Engine 改动

### 6.1 RouteKey

```cpp
// engine/engine.h private:
std::size_t RouteKey(std::string_view key) const noexcept;

// engine/engine.cpp
std::size_t Engine::RouteKey(std::string_view key) const noexcept {
    (void)key;  // P1 不路由——永远返回 0
    return 0;
}
```

P3 多 device 时改为 `return cabe::util::Hash(key) % devices_.size();`。

### 6.2 Engine::Open 补入设备大小查询 + FreeList 初始化

```cpp
#include <sys/ioctl.h>
#include <linux/fs.h>   // BLKGETSIZE64

// 在 Open 的 for 循环内，fd 打开后：

// 查询设备大小
std::uint64_t dev_bytes = 0;
if (::ioctl(fd, BLKGETSIZE64, &dev_bytes) < 0) {
    CABE_LOG_ERROR("ioctl BLKGETSIZE64 失败: fd=%d", fd);
    // 清理已打开的 fd...
    return Status::Error(err::kIoBase);
}
std::uint64_t block_count = (dev_bytes - kDataRegionOffset) / kValueSize;  // P5：扣除头部 8K 超级块；尾部不足 1 MiB 丢弃
if (block_count == 0) {
    CABE_LOG_ERROR("设备太小: %llu 字节 < 1 MiB", (unsigned long long)dev_bytes);
    // 清理...
    return Status::Error(err::kEngineInvalidOpts);
}

DeviceContext dc;
dc.fd = fd;
dc.pool = BufferPool(kDefaultPoolBlocks);
dc.free_list.Init(0, block_count);  // device_id = 0（P1 单设备）
devices_.push_back(std::move(dc));
```

### 6.3 DeviceContext 补入字段

```cpp
// engine/device_context.h
#include "engine/buffer_pool.h"
#include "engine/free_list.h"
#include "engine/meta_index.h"

struct DeviceContext {
    int fd = -1;
    BufferPool pool{0};
    FreeList free_list;
    MetaIndex meta_index;
};
```

---

## 7. 错误码扩展

```cpp
// common/error_code.h 新增

// ---- engine 段（P1M3 新增）----
inline constexpr int kEngineNoSpace = InSeg(kEngineBase, 5);   // -104005

// ---- index 段（P1M3 新增）----
inline constexpr int kIndexKeyNotFound = InSeg(kIndexBase, 0); // -102000

// 越段守护
static_assert(kEngineNoSpace > kEngineBase - kSegmentSize);
static_assert(kIndexKeyNotFound > kIndexBase - kSegmentSize);
```

| 码 | 值 | 含义 | 触发场景 |
|---|---|---|---|
| `kEngineNoSpace` | -104005 | 设备写满 | FreeList 空时 Allocate |
| `kIndexKeyNotFound` | -102000 | key 不存在 | MetaIndex Lookup / Delete |

---

## 8. 测试设计

### 8.1 FreeList 单元测试（`free_list_test.cpp`）

纯内存——不需要设备：

| 用例 | 验证 |
|---|---|
| `InitFillsAll` | `Init(0, 100)` → `available() == 100` |
| `AllocateReturnsSequential` | 连续 Allocate → BlockId 的 `block_idx()` 为 0, 1, 2, ...（因倒序压入 + LIFO pop） |
| `AllocateExhausted` | Allocate 所有块 → 再 Allocate 返回 `kEngineNoSpace` |
| `FreeRestores` | Allocate → Free → Allocate 返回同一 BlockId |
| `DeviceIdPreserved` | `Init(dev=5, count=3)` → Allocate → `bid.dev() == 5` |
| `TailDiscarded` | 验证理念：`block_count = (dev_bytes - kDataRegionOffset) / kValueSize` 向下取整（P5；调用方传入——FreeList 本身不算，测 Init 传入的值即可） |
| `MoveConstruct` | move 后原对象 empty，新对象保留块 |

### 8.2 MetaIndex 单元测试（`meta_index_test.cpp`）

纯内存：

| 用例 | 验证 |
|---|---|
| `InsertAndLookup` | Insert → Lookup 返回 kSuccess + 字段一致 |
| `LookupNotFound` | 查不存在 key → `kIndexKeyNotFound` |
| `InsertOverwrites` | 同 key 两次 Insert → Lookup 返回后一次的 meta |
| `DeleteExisting` | Insert → Delete → Lookup 返回 `kIndexKeyNotFound` |
| `DeleteNotFound` | Delete 不存在 key → `kIndexKeyNotFound` |
| `SizeAndContains` | Insert 3 个 key → `Size() == 3` + `Contains("k1") == true` |
| `MoveConstruct` | move 后原 Size == 0，新对象保留 |

---

## 9. CMake 改动

**`engine/CMakeLists.txt`**：
```cmake
add_library(cabe_engine STATIC
    engine.cpp
    buffer_pool.cpp
    io.cpp
    free_list.cpp
    meta_index.cpp
)
```

**`test/CMakeLists.txt`** 新增：
```cmake
add_executable(test_free_list engine/free_list_test.cpp)
target_link_libraries(test_free_list PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_free_list DISCOVERY_TIMEOUT 60)

add_executable(test_meta_index engine/meta_index_test.cpp)
target_link_libraries(test_meta_index PRIVATE cabe::engine GTest::gtest_main)
gtest_discover_tests(test_meta_index DISCOVERY_TIMEOUT 60)
```

---

## 10. 退出条件与验证步骤

### 10.1 退出条件

1. **FreeList 实装**：`engine/free_list.{h,cpp}` 就位；Init / Allocate / Free + LIFO 语义 + move。
2. **MetaIndex 实装**：`engine/meta_index.{h,cpp}` 就位；Insert / Lookup / Delete / Size / Contains + move。
3. **DeviceContext 补入**：含 `FreeList free_list` + `MetaIndex meta_index`。
4. **Engine 改动**：`RouteKey` 返回 0；`Open` 查设备大小 + 初始化 FreeList。
5. **四档全绿**：`run-tests.sh` 分别 `--asan` / `--tsan` / `--ubsan` / `--release` 跑通。
6. **覆盖率**：`run-coverage.sh --strict` ≥ 80%。

### 10.2 验证步骤

```bash
# FreeList + MetaIndex 纯内存测试（不需要设备）
./scripts/run-tests.sh --filter 'FreeList|MetaIndex'

# Engine 设备测试（需要 CABE_TEST_DEVICE）
./scripts/mkloop.sh create
export CABE_TEST_DEVICE=/dev/loopN
./scripts/run-tests.sh

# 四档回归
./scripts/run-tests.sh --asan
./scripts/run-tests.sh --tsan
./scripts/run-tests.sh --ubsan
./scripts/run-tests.sh --release

# 覆盖率
./scripts/run-coverage.sh --strict

# 清理
./scripts/mkloop.sh cleanup
```

---

## 11. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| FreeList 128 GiB 设备内存占用 | 131072 × 8 字节 = 1 MiB vector | 可接受；P4.5 改造时可优化 |
| MetaIndex string 拷贝 | Insert / Lookup / Delete 每次都 `std::string(key)` | P1 单线程不是瓶颈；P3 透明查找优化 |
| FreeList 无重复归还校验 | 同一 BlockId 多次 Free → 后续分配出重复块 → 数据损坏 | P1 信任内部代码；P4.5 加 debug 校验 |
| ioctl BLKGETSIZE64 在非块设备上失败 | loop 设备 OK；普通文件需 `lseek(SEEK_END)` | P1 用 loop 设备测试；Engine::Open 已限定 O_DIRECT 块设备 |
| MetaIndex Insert 覆盖旧 meta 不释放旧块 | 旧 BlockId 对应的物理块未归还 FreeList | Engine 层（P1M4）负责先 Free 旧块再 Insert 新 meta |

---

## 12. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P1M4** | Put：`FreeList::Allocate` → `WriteBlock` → `MetaIndex::Insert`；Get：`MetaIndex::Lookup` → `ReadBlock`；Delete：`MetaIndex::Delete` → `FreeList::Free`——三路径的组件全部就位 |
| **P3** | `MetaIndex` 的 4 方法签名可直接被 concept 抽象：`Insert(key, meta) → int32_t` / `Lookup(key, out) → int32_t` / `Delete(key) → int32_t` / `Size() → size_t` |
| **P4.5** | `FreeList` 的 `Allocate` / `Free` 可直接被三容器轮换替代——接口不变，内部数据结构改 |

---

**全文完。**
