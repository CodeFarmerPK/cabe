# Cabe P1-M4 设计：Put / Get / Delete 端到端打通

> 本里程碑把 M1–M3 的全部组件（Engine 骨架 + BufferPool + I/O + FreeList + MetaIndex +
> RouteKey）串联成完整的 Put / Get / Delete 路径，替换空壳实现。同时编写端到端测试覆盖
> 正常流程与边界场景。
>
> **无新架构决策**——所有组件接口在 M1–M3 已就位，本里程碑只做串联与测试。
> **本文为详细设计**；其中 C++ 片段为设计示意，代码实装阶段以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P1 / M4 |
| 状态 | **✅ 已锁定（P1M5 收敛）**
| 上游依赖 | P1M1（Engine 骨架）、P1M2（BufferPool + I/O）、P1M3（FreeList + MetaIndex + RouteKey） |
| 下游依赖本里程碑 | P1M5（微基准基线 + P1 收敛） |
| 关联约束 | 返回值分层约定（公开 API 用 Status，内部用 int32_t）；Delete 标记删除 + 立即回收（不含 TRIM） |
| 退出判定 | 见 §8（六条） |

---

## 1. 目标与范围

### 1.1 目标

1. 实装 `Engine::Put`：分配块 → 写设备 → CRC32 → 更新索引（覆盖写时先释放旧块）。
2. 实装 `Engine::Get`：查索引 → 读设备 → CRC32 校验 → 填出参。
3. 实装 `Engine::Delete`：标记删除 + 立即回收块号 + 从索引移除。
4. 编写端到端测试（需 loop 设备）：正常流程 + 覆盖写 + 删后读 + 写满 + 删后再写 + CRC 校验。
5. 新增 2 个 engine 段错误码。

### 1.2 交付范围

1. **`engine/engine.cpp` 修改**：Put / Get / Delete 三路径完整实装（替换空壳）。
2. **`common/error_code.h` 修改**：新增 `kEnginePoolExhausted` / `kEngineDataCorrupted`。
3. **`test/engine/engine_test.cpp` 修改**：新增端到端用例（需 CABE_TEST_DEVICE）。
4. **原有空壳用例清理**：`PutReturnsNotImplemented` / `GetReturnsNotImplemented` /
   `DeleteReturnsNotImplemented` 删除或替换为真实端到端用例。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| TRIM（物理设备块回收） | **P4.5**（FreeList 改造 + io_uring 异步 TRIM） | P1 无持久化 + 不影响正确性；P4.5 FreeList 三容器轮换与 TRIM 天然耦合 |
| GC 线程（数据空洞回收） | **P7**（Reactor + 多线程） | 需多线程基础设施 |
| WAL 持久化 Delete 帧 | **P5** | P1 无持久化——Delete 只改 RAM 索引 |

---

## 2. 现状盘点

- **Engine::Put / Get / Delete**：当前是空壳，前置校验已实装（is_open / key 非空 / value 大小），
  路径体返回 `kEngineNotImplemented`。
- **全部组件已就位**：
  - `FreeList::Allocate(BlockId* out) → int32_t` / `Free(BlockId)`
  - `MetaIndex::Insert(key, meta)` / `Lookup(key, out*)` / `Delete(key)` → `int32_t`
  - `BufferPool::Allocate() → std::byte*` / `Free(buf)`
  - `WriteBlock(fd, block_idx, buf)` / `ReadBlock(fd, block_idx, buf)` → `int32_t`
  - `RouteKey(key) → 0`（P1 单设备）
  - `util::CRC32(DataView)` / `util::GetWallTimeNs()`
- **DeviceContext** 含 `fd` / `pool` / `free_list` / `meta_index`——Engine 直接访问（struct 公开成员）。

---

## 3. 三条路径详细设计

### 3.1 Put 路径

```cpp
Status Engine::Put(std::string_view key, DataView value) {
    // ---- 前置校验（已有）----
    if (!opened_) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);

    auto& dc = devices_[RouteKey(key)];

    // ---- 覆盖写处理：如果 key 已存在，先释放旧块 ----
    ValueMeta old_meta{};
    int32_t lookup_rc = dc.meta_index.Lookup(key, &old_meta);
    if (lookup_rc == err::kSuccess) {
        dc.free_list.Free(old_meta.block);
    }
    // lookup_rc == kIndexKeyNotFound → 新 key，无需释放

    // ---- 分配新块 ----
    BlockId block_id{};
    int32_t rc = dc.free_list.Allocate(&block_id);
    if (rc != err::kSuccess) return Status::Error(rc);  // kEngineNoSpace

    // ---- 分配对齐 buffer + 填 value ----
    std::byte* buf = dc.pool.Allocate();
    if (!buf) return Status::Error(err::kEnginePoolExhausted);
    std::memcpy(buf, value.data(), kValueSize);

    // ---- 写设备 ----
    rc = WriteBlock(dc.fd, block_id.block_idx(), buf);
    dc.pool.Free(buf);
    if (rc != err::kSuccess) {
        // 写失败：归还已分配的块号
        dc.free_list.Free(block_id);
        return Status::Error(rc);
    }

    // ---- 构造 ValueMeta + 更新索引 ----
    ValueMeta meta{};
    meta.block = block_id;
    meta.timestamp = util::GetWallTimeNs();
    meta.crc = util::CRC32(value);
    meta.state = ValueState::Active;
    dc.meta_index.Insert(key, meta);

    return Status::Ok();
}
```

**设计要点**：
- **覆盖写**：先 Lookup 旧 meta → 成功则 Free 旧块。MetaIndex::Insert 覆盖旧条目。
- **失败回滚**：WriteBlock 失败 → Free 已分配的块号（避免块泄漏）。
- **BufferPool 先分配后写**：确保有对齐 buffer 才做 I/O；写完立即 Free（不持有跨操作）。
- **CRC32**：对 value 原始数据计算（不含 meta），存入 ValueMeta。
- **timestamp**：用 `GetWallTimeNs()`——墙钟纳秒，P5 WAL 需要单调递增但 P1 不关心。

### 3.2 Get 路径

```cpp
Status Engine::Get(std::string_view key, DataBuffer value) {
    if (!opened_) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);
    if (value.size() != kValueSize) return Status::Error(err::kEngineInvalidValue);

    auto& dc = devices_[RouteKey(key)];

    // ---- 查索引 ----
    ValueMeta meta{};
    int32_t rc = dc.meta_index.Lookup(key, &meta);
    if (rc != err::kSuccess) return Status::Error(rc);  // kIndexKeyNotFound

    // ---- 分配对齐 buffer + 读设备 ----
    std::byte* buf = dc.pool.Allocate();
    if (!buf) return Status::Error(err::kEnginePoolExhausted);

    rc = ReadBlock(dc.fd, meta.block.block_idx(), buf);
    if (rc != err::kSuccess) {
        dc.pool.Free(buf);
        return Status::Error(rc);
    }

    // ---- CRC32 校验 ----
    uint32_t crc_check = util::CRC32(DataView{buf, kValueSize});
    if (crc_check != meta.crc) {
        CABE_LOG_ERROR("CRC32 不匹配: key 的存储 crc=0x%08X, 读出 crc=0x%08X",
                       meta.crc, crc_check);
        dc.pool.Free(buf);
        return Status::Error(err::kEngineDataCorrupted);
    }

    // ---- 填出参 ----
    std::memcpy(value.data(), buf, kValueSize);
    dc.pool.Free(buf);
    return Status::Ok();
}
```

**设计要点**：
- **CRC32 校验**：读出数据后计算 CRC32 与 MetaIndex 存的 `meta.crc` 比对——不匹配说明设备数据损坏。
- **BufferPool 用完即 Free**——不持有跨操作。

### 3.3 Delete 路径

```cpp
Status Engine::Delete(std::string_view key) {
    if (!opened_) return Status::Error(err::kEngineNotOpen);
    if (key.empty()) return Status::Error(err::kMemEmptyKey);

    auto& dc = devices_[RouteKey(key)];

    // ---- 查索引 ----
    ValueMeta meta{};
    int32_t rc = dc.meta_index.Lookup(key, &meta);
    if (rc != err::kSuccess) return Status::Error(rc);  // kIndexKeyNotFound

    // ---- 立即回收块号 ----
    dc.free_list.Free(meta.block);

    // ---- 从索引移除 ----
    dc.meta_index.Delete(key);

    // 不做 I/O、不发 TRIM（P1 无持久化；TRIM 在 P4.5 引入）。
    // 物理块上的旧数据残留，由索引控制数据可见性。

    return Status::Ok();
}
```

**设计要点**：
- **标记删除 + 立即回收**：FreeList 回收块号 + MetaIndex 移除条目——两步原子性由 P1 单线程保证。
- **不做 I/O**：旧数据在设备上残留（P1 "无持久化"语义——索引即真相）。
- **不发 TRIM**：P4.5 FreeList 改造时引入异步 TRIM。

---

## 4. 错误码扩展

```cpp
// common/error_code.h 新增（engine 段）
inline constexpr int kEnginePoolExhausted = InSeg(kEngineBase, 6);  // -104006
inline constexpr int kEngineDataCorrupted = InSeg(kEngineBase, 7);  // -104007

static_assert(kEngineDataCorrupted > kEngineBase - kSegmentSize);
```

| 码 | 值 | 触发场景 |
|---|---|---|
| `kEnginePoolExhausted` | -104006 | Put / Get 时 BufferPool 满（P1 单线程 16 块几乎不触发——防御性兜底） |
| `kEngineDataCorrupted` | -104007 | Get 读出数据的 CRC32 与索引存的不匹配——设备数据损坏 |

---

## 5. 需要引入的头文件

`engine/engine.cpp` 新增：
```cpp
#include "engine/io.h"        // WriteBlock / ReadBlock
#include "util/crc32.h"       // util::CRC32
#include "util/util.h"        // util::GetWallTimeNs
#include <cstring>            // std::memcpy
```

---

## 6. 测试设计

### 6.1 原有空壳用例处理

| 旧用例 | 处理 |
|---|---|
| `PutReturnsNotImplemented` | 删除（Put 不再返回 kEngineNotImplemented） |
| `GetReturnsNotImplemented` | 删除 |
| `DeleteReturnsNotImplemented` | 删除 |
| `PutEmptyKeyFails` / `PutWrongValueSizeFails` | 保留（前置校验仍生效） |

### 6.2 新增端到端用例（全部需要 CABE_TEST_DEVICE）

| 用例 | 验证 |
|---|---|
| `PutGetRoundTrip` | Put 1 MiB（填充 0xAB）→ Get → 逐字节比对一致 |
| `PutGetMultipleKeys` | Put 3 个不同 key → Get 各自返回正确 value |
| `PutOverwrite` | 同 key 两次 Put 不同 value → Get 返回后一次的 |
| `DeleteThenGetFails` | Put → Delete → Get 返回 `kIndexKeyNotFound` |
| `DeleteNotFound` | Delete 不存在 key → `kIndexKeyNotFound` |
| `GetNotFound` | Get 不存在 key → `kIndexKeyNotFound` |
| `PutUntilFull` | 连续 Put 不同 key 直到 FreeList 耗尽 → 返回 `kEngineNoSpace` |
| `DeleteFreesThenPutAgain` | Put 填满 → Delete 一个 → 再 Put 成功 |
| `CRC32Verified` | Put → 用 `pwrite` 手动篡改设备上的 value 数据 → Get 返回 `kEngineDataCorrupted` |

### 6.3 测试辅助

端到端测试用例需要小设备（用少量块即可覆盖"写满"场景）：
```bash
SIZE_MB=4 ./scripts/mkloop.sh create    # 4 MiB = 4 个块——快速写满
export CABE_TEST_DEVICE=/dev/loopN
```

但 `PutGetMultipleKeys` 等需要多块——4 块可能太小。折中用 **8 MiB = 8 块**：写满测试 Put 8 个 key
即满；正常测试 3-4 个 key 有余量。

---

## 7. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 覆盖写先 Free 旧块再 Allocate 新块 | 如果 FreeList 只剩 1 块：Free 旧 → Allocate 新 → OK。但如果 Free 旧后 Allocate 的是刚 Free 的那个块号 → 写新数据覆盖旧数据 → 如果写失败旧数据也没了 | P1 单线程 + 无持久化——可接受；P5 WAL 保证 crash safety |
| BufferPool 耗尽返回 `kEnginePoolExhausted` | P1 单线程 16 块——Put/Get 只用 1 块就 Free——几乎不触发 | 防御性兜底；P7 多线程时池可能真满 |
| CRC32 校验假阳性 | CRC32C 碰撞概率 2^-32——极低 | 可接受；P5+ 可加 xxh3 二次校验 |
| Delete 不发 TRIM | 旧数据残留在 SSD NAND 上 → 写放大 + 寿命损耗 | P4.5 FreeList 改造时加异步 TRIM；P1 用 loop 设备测试无影响 |
| Delete 不写 WAL | P1 无持久化——Delete 信息重启后丢失 | P5 WAL 引入 Delete 帧持久化 |

---

## 8. 退出条件与验证步骤

### 8.1 退出条件

1. **Put 完整路径**：分配块 → 写设备 → CRC32 → 更新索引 + 覆盖写释放旧块。
2. **Get 完整路径**：查索引 → 读设备 → CRC32 校验 → 填出参。
3. **Delete 完整路径**：标记删除 + 立即回收 + 索引移除。
4. **端到端测试**：9 个用例全绿（需 CABE_TEST_DEVICE）。
5. **四档全绿**：`run-tests.sh --asan` / `--tsan` / `--ubsan` / `--release` 跑通。
6. **覆盖率**：`run-coverage.sh --strict` ≥ 80%。

### 8.2 验证步骤

```bash
# 创建小 loop 设备（8 MiB = 8 块，适合写满测试）
SIZE_MB=8 ./scripts/mkloop.sh create
export CABE_TEST_DEVICE=/dev/loopN

# 端到端测试
./scripts/run-tests.sh --filter 'Engine'

# 四档回归
./scripts/run-tests.sh --asan
./scripts/run-tests.sh --tsan
./scripts/run-tests.sh --ubsan
./scripts/run-tests.sh --release

# 覆盖率
unset CABE_TEST_DEVICE
./scripts/run-coverage.sh --strict

# 清理
./scripts/mkloop.sh cleanup
```

---

## 9. 对下游里程碑的接口承诺

| 下游 | 本里程碑提供的接入点 |
|---|---|
| **P1M5** | Put / Get / Delete 完整路径可用 → 微基准可跑 Put/Get 吞吐 + 延迟 |
| **P2** | Engine 公开 API 签名已定型（`Status Put(key, value)` / `Status Get(key, value)` / `Status Delete(key)`）→ P2 冻结只调细节 |
| **P5** | Put 路径的 CRC32 + timestamp 已填入 ValueMeta → WAL 帧可直接引用 meta 字段 |
| **P4.5** | Delete 路径的 FreeList::Free(block) 就是 TRIM 触发点 → P4.5 在此处加异步 TRIM |

---

**全文完。**
