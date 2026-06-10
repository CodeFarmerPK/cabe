# Cabe P3-M2 设计：MetaIndex 抽象层

> 本里程碑定义 MetaIndex 的 C++20 concept（7 个方法），实装 `HashMetaIndex`（包装 P1 的
> `unordered_map`；ForEach / WriteSnapshot / LoadSnapshot 为空壳，P5 实装），并编写契约
> 测试套件（`TYPED_TEST`——未来 P9 B+ 树实现加入 `Types<>` 即可复用同一套用例）。
>
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。
>
> **⚠️ P5M4 起本 concept 已收窄（以 [P5M4 设计稿](../P5/P5M4_snapshot_design.md) 为准）**：移除 `WriteSnapshot` / `LoadSnapshot`（快照读写 I/O 上移到独立 `snapshot/` 模块，后端只保留 `ForEach` + `Insert`）；`ForEach` 改为**返回 `int32_t`、可中途报错中止**（原 `void`）。下文"7 个方法 / `ForEach` 返回 void / `WriteSnapshot` / `LoadSnapshot` 空壳"等描述保留作历史记录。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P3 / M2 |
| 状态 | **✅ 已锁定（P3M4 收敛）** |
| 上游依赖 | P3M1（IoBackend 抽象层已就位——作为目录结构 + C++20 concept 模式的参考） |
| 下游依赖本里程碑 | P3M3（Engine 切换——用 MetaIndex concept 替代直接调 `unordered_map`） |
| 退出判定 | 见 §8 |

---

## 1. 目标与范围

### 1.1 目标

1. 定义 MetaIndex 的 C++20 concept：7 个方法（Insert / Lookup / Delete / Size / Contains / ForEach / WriteSnapshot / LoadSnapshot）。
2. 实装 `HashMetaIndex`：包装 P1 的 `unordered_map<string, ValueMeta>`；ForEach / WriteSnapshot / LoadSnapshot 为空壳（返回 `kEngineNotImplemented`），P5 实装。
3. 建立 `index/` 目录结构（与 `io/` 对称：接口在顶层、实现在子目录）。
4. 编写契约测试套件（`TYPED_TEST`——任何 MetaIndex 实现都跑同一套用例）。
5. P1 的 `engine/meta_index.h` / `engine/meta_index.cpp` 在 P3M3 Engine 切换后可删除——本里程碑先保留。

### 1.2 交付范围

1. **`index/meta_index.h`**：MetaIndex C++20 concept。
2. **`index/hash/hash_meta_index.h` + `index/hash/hash_meta_index.cpp`**：HashMetaIndex 类。
3. **`index/CMakeLists.txt`**：index 模块。
4. **根 `CMakeLists.txt` 修改**：加 `add_subdirectory(index)`。
5. **`test/index/meta_index_contract_test.cpp`**：契约测试套件。
6. **`test/CMakeLists.txt` 修改**：注册新测试。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| Engine 切换到 MetaIndex concept | **P3M3** | 本里程碑只做接口 + 实现 |
| ForEach / WriteSnapshot / LoadSnapshot 实装 | **P5** | P3 写空壳占位 |
| B+ 树实现 | **P9** | 加 `index/bplustree/` 子目录 + 改 `Types<>` |
| 删除 engine/meta_index.h / .cpp | **P3M3** | Engine 切换后旧代码不再需要 |

---

## 2. 决策汇总

| 编号 | 决策 | 结果 |
|---|---|---|
| **P3M2-D1** | 目录布局 | `index/meta_index.h`（接口）+ `index/hash/hash_meta_index.*`（实现）——与 `io/` 对称 |
| **P3M2-D2** | 契约测试 | `TYPED_TEST`——同一套用例覆盖所有实现；P9 加 B+ 树只需改 `Types<>` 列表 |

---

## 3. MetaIndex C++20 concept

```cpp
// index/meta_index.h
#ifndef CABE_META_INDEX_CONCEPT_H
#define CABE_META_INDEX_CONCEPT_H

#include "common/structs.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cabe {

    using MetaIndexVisitor = std::function<void(std::string_view key, const ValueMeta& meta)>;

    template<typename T>
    concept MetaIndexBackend = requires(T& idx, const T& cidx,
                                        std::string_view key,
                                        const ValueMeta& meta,
                                        ValueMeta* out,
                                        MetaIndexVisitor visitor,
                                        const std::string& path) {
        { idx.Insert(key, meta) } -> std::same_as<int32_t>;
        { cidx.Lookup(key, out) } -> std::same_as<int32_t>;
        { idx.Delete(key) } -> std::same_as<int32_t>;
        { cidx.Size() } -> std::convertible_to<std::size_t>;
        { cidx.Contains(key) } -> std::same_as<bool>;
        { cidx.ForEach(visitor) } -> std::same_as<void>;
        { cidx.WriteSnapshot(path) } -> std::same_as<int32_t>;
        { idx.LoadSnapshot(path) } -> std::same_as<int32_t>;
    };

} // namespace cabe

#endif // CABE_META_INDEX_CONCEPT_H
```

**设计要点**：
- `MetaIndexVisitor` 用 `std::function` 包装回调——简洁、契约测试可直接用 lambda。
- `Lookup` / `Size` / `Contains` / `ForEach` / `WriteSnapshot` 约束在 `const T&` 上——读操作不修改状态。
- `LoadSnapshot` 在 `T&` 上——加载快照会替换内部状态。
- 全部返回 `int32_t`（按返回值分层约定）；`ForEach` 返回 `void`（遍历不失败——内部状态已在 RAM）。

---

## 4. HashMetaIndex 设计

### 4.1 类声明

```cpp
// index/hash/hash_meta_index.h
#ifndef CABE_HASH_META_INDEX_H
#define CABE_HASH_META_INDEX_H

#include "index/meta_index.h"
#include "common/error_code.h"
#include "common/structs.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>

namespace cabe {

    class HashMetaIndex {
    public:
        HashMetaIndex() = default;

        int32_t Insert(std::string_view key, const ValueMeta& meta);
        int32_t Lookup(std::string_view key, ValueMeta* out) const;
        int32_t Delete(std::string_view key);
        std::size_t Size() const noexcept;
        bool Contains(std::string_view key) const;

        // 空壳——P5 实装
        void ForEach(MetaIndexVisitor visitor) const;
        int32_t WriteSnapshot(const std::string& path) const;
        int32_t LoadSnapshot(const std::string& path);

    private:
        std::unordered_map<std::string, ValueMeta> map_;
    };

    static_assert(MetaIndexBackend<HashMetaIndex>);

} // namespace cabe

#endif // CABE_HASH_META_INDEX_H
```

### 4.2 实现要点

Insert / Lookup / Delete / Size / Contains——与 P1 `engine/meta_index.cpp` 逻辑完全相同，只是类名从 `MetaIndex` 改为 `HashMetaIndex`。

**ForEach（空壳）**：
```cpp
void HashMetaIndex::ForEach(MetaIndexVisitor visitor) const {
    for (const auto& [key, meta] : map_) {
        visitor(key, meta);
    }
}
```

注：ForEach 虽标"空壳"但遍历逻辑本身很简单——直接实装。真正的空壳是 WriteSnapshot / LoadSnapshot。

**WriteSnapshot / LoadSnapshot（空壳）**：
```cpp
int32_t HashMetaIndex::WriteSnapshot(const std::string& path) const {
    (void)path;
    return err::kEngineNotImplemented;
}

int32_t HashMetaIndex::LoadSnapshot(const std::string& path) {
    (void)path;
    return err::kEngineNotImplemented;
}
```

---

## 5. 目录与 CMake

### 5.1 目录结构

```
index/
├── CMakeLists.txt              # index 模块
├── meta_index.h                # C++20 concept
└── hash/
    ├── hash_meta_index.h       # HashMetaIndex 声明
    └── hash_meta_index.cpp     # HashMetaIndex 实现

test/index/
└── meta_index_contract_test.cpp  # 契约测试（TYPED_TEST）
```

### 5.2 CMake

**`index/CMakeLists.txt`**：
```cmake
add_library(cabe_index STATIC hash/hash_meta_index.cpp)
target_link_libraries(cabe_index PUBLIC cabe_common)
add_library(cabe::index ALIAS cabe_index)
```

**根 `CMakeLists.txt`**：加 `add_subdirectory(index)`。

**`test/CMakeLists.txt`**：
```cmake
add_executable(test_meta_index_contract index/meta_index_contract_test.cpp)
target_link_libraries(test_meta_index_contract PRIVATE cabe::index GTest::gtest_main)
gtest_discover_tests(test_meta_index_contract DISCOVERY_TIMEOUT 60)
```

### 5.3 依赖链

```
cabe_common (INTERFACE)
  ├─► cabe_io (STATIC)       ← P3M1
  └─► cabe_index (STATIC)    ← P3M2 新增
```

`cabe_index` 不依赖 `cabe_io` 也不依赖 `cabe_engine`——独立模块。P3M3 Engine 切换时 `cabe_engine` 链接 `cabe_io` + `cabe_index`。

---

## 6. 契约测试设计

```cpp
// test/index/meta_index_contract_test.cpp

template<typename T>
class MetaIndexContractTest : public ::testing::Test {
protected:
    T index_;
};

using MetaIndexImpls = ::testing::Types<cabe::HashMetaIndex>;
TYPED_TEST_SUITE(MetaIndexContractTest, MetaIndexImpls);
```

**用例清单**（全部用 `TYPED_TEST`——P9 加 B+ 树自动复用）：

| 用例 | 验证 |
|---|---|
| `InsertAndLookup` | Insert → Lookup 返回 kSuccess + 字段一致 |
| `LookupNotFound` | 查不存在 key → kIndexKeyNotFound |
| `InsertOverwrites` | 同 key 两次 Insert → Lookup 返回后一次的 meta |
| `DeleteExisting` | Insert → Delete → Lookup 返回 kIndexKeyNotFound |
| `DeleteNotFound` | Delete 不存在 key → kIndexKeyNotFound |
| `SizeAndContains` | Insert 3 个 key → Size() == 3 + Contains 正确 |
| `ForEachVisitsAll` | Insert 3 个 key → ForEach 收集到 3 个条目 |
| `WriteSnapshotStub` | WriteSnapshot 返回 kEngineNotImplemented（空壳验证） |
| `LoadSnapshotStub` | LoadSnapshot 返回 kEngineNotImplemented（空壳验证） |
| `ConceptSatisfied` | `static_assert(MetaIndexBackend<HashMetaIndex>)` 编译通过 |

---

## 7. 风险与权衡

| 风险 | 缓解 |
|---|---|
| ForEach 用 `std::function` 有间接调用开销 | P1-P4 单线程不是瓶颈；P9 B+ 树性能敏感时可改模板回调 |
| WriteSnapshot / LoadSnapshot 空壳——签名 P5 可能要调 | 接口可扩展（冻结是相对的） |
| 与 P1 `engine/meta_index.*` 并行存在 | P3M3 切换后删旧的 |

---

## 8. 退出条件

1. **接口定义就位**：`index/meta_index.h` 含 7 个方法的 C++20 concept。
2. **HashMetaIndex 实装**：`index/hash/hash_meta_index.*` + `static_assert(MetaIndexBackend<HashMetaIndex>)` 通过。
3. **契约测试**：10 个用例全绿。
4. **原有 75 个用例不退步**。
5. **四档全绿**。
6. **覆盖率** ≥ 80%。

---

## 9. 对下游里程碑的接口承诺

| 下游 | 接入点 |
|---|---|
| **P3M3** | `HashMetaIndex` 替换 DeviceContext 里的 `MetaIndex`；Engine 通过 MetaIndexBackend concept 调用 |
| **P5** | ForEach + WriteSnapshot + LoadSnapshot 实装 |
| **P9** | `index/bplustree/` 子目录 + 改契约测试 `Types<>` |

---

**全文完。**
