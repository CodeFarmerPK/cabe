# Cabe P0-M4 设计：`util/hash.{h,cpp}` xxh3 路由 hash 接入

> 本里程碑引入**路由 hash**：用 xxh3 实现 `cabe::util::Hash`，并在其上提供 `RouteToDevice`
> （D7：`device = hash(key) % N` 的实装入口）。**xxh3 仅用于路由，与数据完整性校验的
> CRC32C（`util/crc32`，D14）严格分工、互不混用**。
> **本文为详细设计，暂不生成代码**；其中 C++ 片段均为设计示意。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M4 |
| 状态 | **✅ 已锁定（P0M7 收敛）** |
| 上游依赖 | M1（构建骨架、`cabe_util`）、M2（`DataView`=`span<const std::byte>`、`DeviceId`） |
| 下游依赖本里程碑 | M5（hash 单测：已知向量 + 分布）、P2/P7（`device = hash%N` 路由）、P11（多 device 负载均衡测量） |
| 关联架构决策 | D6（路由 hash = xxh3，v2.0 前**冻结**）、D7（`device_idx = hash(key) % N`）、D14（CRC32C 管完整性，xxh3 仅路由） |
| 退出判定 | 已知向量测试通过；100K 随机 key 的 `RouteToDevice` 分布卡方检验通过；双工具链构建零警告 |

---

## 1. 目标与范围

### 1.1 目标

1. 引入 xxh3，实现 `cabe::util::Hash(DataView)` / `Hash(std::string_view)`，结果在 v2.0 前**冻结**（同一输入跨版本/跨环境恒定输出）。
2. 提供 `cabe::util::RouteToDevice(key, n_devices)`，作为 D7 路由的唯一实装入口。
3. 明确并锁定 **xxh3（路由）与 CRC32C（完整性）的分工**，杜绝混用。

### 1.2 交付范围（本里程碑产出）

1. 引入 xxhash（引入方式见 §3/§4 决策）。
2. 新增 `util/hash.h`：声明 `Hash` 两个重载 + `RouteToDevice`（不暴露 xxhash 头）。
3. 新增 `util/hash.cpp`：包含 xxhash、实现上述函数。
4. `util/CMakeLists.txt`：把 `hash.cpp` 加入 `cabe_util`（M1 已留加入点注释）。

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| hash 单元测试（已知向量、分布卡方、跨平台稳定性） | **M5** | M4 只实装，测试框架在 M5 接入 |
| hash 微基准（吞吐）归档 | **M7**（`bench/baselines/p0_utilities.json`） | bench 框架 M5 接入、基线 M7 归档 |
| 两级路由的 reactor 维度 `reactor = (hash/N) % R` | **P7** | M4 只做 device 级（D7）；reactor 级属并发模型 |
| `Hash` 的 128-bit 变体 / 带 seed 变体 | 需要时再加 | M4 路由只需 64-bit、固定 seed |
| README 依赖列表 / `setup-dev.sh` 随选型调整 | **M7 收敛 / owner** | M4 不擅改这两份；§4 注明应同步项 |

---

## 2. 现状盘点（读码结论）

- **无 hash 模块**：`util/` 下只有 `crc32`、`cpu_features`、`util.h`。
- **M1 已留接入点**：`util/CMakeLists.txt` 内有注释 `# hash.cpp        # ← M4 在此加入 xxh3 路由 hash`。
- **README 仓库结构**已登记 `util/hash.{h,cpp} — xxh3 路由 hash（P0 内引入）`；依赖列表写有"xxhash 库（P0+，路由 hash）"。
- **`scripts/setup-dev.sh` 当前未安装 xxhash**（`REQUIRED_PKGS` 无 `xxhash-devel`）—— 选型若为系统库则须补装；若为内嵌则无需（见 §4）。
- **可复用上游**：`DataView = std::span<const std::byte>`（M2）直接作 `Hash` 入参；`DeviceId = std::uint8_t`（M2，N ≤ 256）作 `RouteToDevice` 返回。

---

## 3. 关键决策（owner 已裁决）

### 决策-1（核心，ROADMAP 点名 M4 拍板）：xxhash 引入方式 —— 内嵌 single-header vs 系统库

| 维度 | 内嵌 single-header（**已采纳**） | 系统库（`xxhash-devel`，未采纳） |
|---|---|---|
| 与 D6「冻结」 | ✅ 版本钉死在仓库，输出天然冻结 | ⚠️ 版本随发行版浮动，与"冻结"有张力 |
| 路由正确性 | ✅ 跨环境/跨版本同一 key 必同一 device | ⚠️ 库升级若改实现 → 路由漂移 → **数据找不到** |
| 系统依赖 | ✅ 无（`setup-dev.sh` 不改） | 需补 `xxhash-devel` + `find_package`/`pkg_check_modules` |
| 仓库体积 | ⚠️ +1 个 `xxhash.h`（约 270 KB 单头） | ✅ 不入库 |
| 内联优化 | ✅ `XXH_INLINE_ALL` 可整体内联进 `hash.cpp` | 一般动态/静态链接 |
| 安全更新 | 手动升级 vendored 版本 | ✅ 跟随发行版 |

**已采纳（owner 终审通过）：内嵌 single-header（vendored `xxhash.h`，钉死版本，如 v0.8.x）。**
- 决定性理由：**路由 hash 的稳定性是正确性要求**——`hash(key) % N` 决定一个 key 的数据落在哪个 device，
  若 hash 输出因环境/版本变化，同一 key 会路由到不同 device，**已写入的数据将再也读不到**。D6 因此要求"冻结"。
  vendored 固定版本是对"冻结"最直接、最可靠的保证；系统库的版本浮动与此目标相悖。
- XXH3 自 xxHash v0.8.0 起算法已正式 frozen，但 vendored 仍更稳妥（连"依赖发行版恰好 ≥ 0.8.0 且不回退"
  这一假设都不必依赖）。
- 代价（仓库 +1 单头、手动升级）可接受，且 hash 是算法型依赖（非 liburing 那种运行时库），最适合 vendoring。

> 若 owner 倾向系统库：需 ① `setup-dev.sh` 加 `xxhash-devel`；② CMake `pkg_check_modules(libxxhash)`；
> ③ 文档显式声明"依赖 xxHash ≥ 0.8.0"并承担版本漂移风险评估。

### 决策-2：放置位置与命名空间

| 维度 | 内容 |
|---|---|
| vendored 头位置 | **`third_party/xxhash/xxhash.h`**（工程根下新增 `third_party/`；含来源 URL + 版本 + LICENSE 说明） |
| 接口命名空间 | `cabe::util`（与 `crc32`/`cpu_features` 一致；延续命名空间约定） |
| 头暴露 | `hash.h` **只声明** `Hash`/`RouteToDevice`，**不** `#include xxhash.h`；xxhash 仅在 `hash.cpp` 内可见，避免污染所有下游 TU |

---

## 4. 依赖选型细节（采用内嵌 single-header）

- `third_party/xxhash/xxhash.h`：从 xxHash 官方仓库取 `xxhash.h` 单头，固定到某 release tag（如 `v0.8.2`），
  顶部注释记录来源、版本、commit、BSD-2 许可。
- `hash.cpp` 内：
  ```cpp
  #define XXH_INLINE_ALL          // 全部 inline，无需链接 libxxhash；定义集中在本 TU
  #include "third_party/xxhash/xxhash.h"
  ```
- 仅 `hash.cpp` 引入 xxhash 符号；`hash.h` 不含 xxhash，下游只见 `cabe::util::Hash`。
- **不影响** `cabe_common` / 其它目标；只有 `cabe_util` 多编译一个 `hash.cpp`。

---

## 5. 接口设计（`util/hash.h`）

```cpp
#ifndef CABE_HASH_H
#define CABE_HASH_H

#include <cstdint>
#include <string_view>

#include "common/structs.h" // DataView, DeviceId

namespace cabe::util {

    // 路由 hash：xxh3 64-bit，固定 seed（0），v2.0 前冻结（D6）。
    // 仅用于 key 路由，不用于数据完整性（完整性走 util/crc32 的 CRC32C，D14）。
    std::uint64_t Hash(DataView data) noexcept;
    std::uint64_t Hash(std::string_view key) noexcept;

    // D7 路由入口：device_idx = Hash(key) % n_devices。
    // 前置：n_devices ∈ [1, 256]（DeviceId 为 uint8_t，N 上限 256；n_devices==0 是调用方 bug）。
    DeviceId RouteToDevice(std::string_view key, std::size_t n_devices) noexcept;

} // namespace cabe::util

#endif // CABE_HASH_H
```

---

## 6. 实现要点（`util/hash.cpp`）

```cpp
#define XXH_INLINE_ALL
#include "third_party/xxhash/xxhash.h"

#include "hash.h"     // 同目录兄弟，裸名（与 crc32.cpp 风格一致）
#include <cassert>

namespace cabe::util {

    std::uint64_t Hash(DataView data) noexcept {
        return XXH3_64bits(data.data(), data.size());            // seed 0，固定
    }
    std::uint64_t Hash(std::string_view key) noexcept {
        return XXH3_64bits(key.data(), key.size());
    }

    DeviceId RouteToDevice(std::string_view key, std::size_t n_devices) noexcept {
        assert(n_devices >= 1 && n_devices <= 256 && "n_devices must be in [1,256]"); // Debug 防线
        return static_cast<DeviceId>(Hash(key) % n_devices);     // D7
    }

} // namespace cabe::util
```

- **固定 seed = 0、固定 XXH3 64-bit**：保证 D6 冻结。绝不引入运行期可变 seed（会破坏路由稳定）。
- `Hash(DataView)`：`data.data()` 是 `const std::byte*`，`XXH3_64bits` 收 `const void*`，零适配。
- `n_devices == 0` 用 `assert` 拦截（Debug 防线，类比 M2 `BlockId::Make`）；Release 下若传 0 是调用方违约。
- **职责边界（强约束）**：本文件只产路由 hash；任何"数据是否损坏"的判断必须用 `util/crc32`。
  二者在代码与文档层面都不交叉调用。

---

## 7. CMake 连带（`util/CMakeLists.txt`）

把 M1 预留的注释行落实为真实源文件：

```cmake
add_library(cabe_util STATIC
    crc32.cpp
    cpu_features.cpp
    hash.cpp          # ← M4 落实（取代 M1 的占位注释）
)
target_link_libraries(cabe_util PUBLIC cabe_common)
add_library(cabe::util ALIAS cabe_util)
```

- `hash.cpp` 内 `#include "third_party/xxhash/xxhash.h"`：include 根 = `${PROJECT_SOURCE_DIR}`（M1 由 `cabe_common` 提供），故 `third_party/...` 路径可达，**无需新增 include 目录**。
- `cabe_util` 拓扑不变（仍 STATIC，仍 PUBLIC 链 `cabe_common`）；根 `CMakeLists.txt` 不改。
- 若选系统库（决策-1 备选）：此处改为 `target_link_libraries(cabe_util PUBLIC ... PkgConfig::libxxhash)`。

---

## 8. 关键设计决策

| # | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| M4-D1 | xxhash 用**内嵌 single-header**（vendored，钉死版本） | 系统库 `xxhash-devel` | 路由 hash 必须跨环境/版本冻结（D6），否则数据路由漂移丢数据；vendored 是最可靠保证 | **锁定（owner 已终审）**（§3 决策-1） |
| M4-D2 | `Hash` = XXH3 64-bit，**固定 seed 0** | 带 seed / 128-bit | 路由只需 64-bit；固定 seed 保证冻结 | 锁定 |
| M4-D3 | `hash.h` 不暴露 xxhash 头；xxhash 仅在 `hash.cpp` | 头里 `#include xxhash` | 避免 270KB 单头污染所有下游 TU | 锁定 |
| M4-D4 | xxh3 仅路由、CRC32C 仅完整性，互不混用 | 统一一个 hash | D14 明确分工；路由要快、完整性要抗突发错误，诉求不同 | 锁定 |
| M4-D5 | `RouteToDevice` 对 `n_devices` 加 `assert([1,256])` | 不校验 | 防除零 / 越 `DeviceId` 上限（Debug 防线，Release 零成本） | 锁定 |
| M4-D6 | vendored 头置于 `third_party/xxhash/`，命名空间 `cabe::util` | 放 util/ 下 | 第三方与自有代码分目录；命名空间与 util 一致 | 锁定 |

---

## 9. 与 ROADMAP / 决策一致性核对

| ROADMAP M4 / D 决策 | 本设计 | 状态 |
|---|---|---|
| 决策子项：系统库 vs 内嵌 single-header（M4 拍板，写入 design） | §3 决策-1 内嵌（已采纳） | ✅ |
| 接口 `Hash(DataView)` / `Hash(string_view)` | §5 | ✅ |
| 路由 `RouteToDevice(key, n_devices)`（D7 入口） | §5 / §6 | ✅ |
| D6：路由 hash = xxh3，冻结 | §6 固定 XXH3 + seed 0 + vendored | ✅ |
| D14：xxh3 仅路由，CRC32C 管完整性 | §6 职责边界（M4-D4） | ✅ |
| 退出：已知向量 + 100K 分布卡方 | §11 | ✅（M5 固化） |

---

## 10. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| 路由漂移导致数据丢失 | hash 输出若跨环境变化，key→device 改变 | M4-D1 vendored 钉死版本 + 固定 seed/算法；M5 跨平台稳定性测试 |
| vendored 头维护 | 安全更新需手动升级 | 头顶注明版本/来源；升级走 review，且需确认 XXH3 输出不变 |
| `DeviceId` 上限 256 | N > 256 时 `RouteToDevice` 截断 | D5 已限定 device_id 8 位（N ≤ 256）；`assert` 兜底 |
| 分布偏斜 | 真实 key 分布下负载不均 | M5 卡方检验；P11 真实 key 分布负载偏斜测量 |
| `XXH3_64bits` 对空输入 | `Hash("")` 合法（XXH3 有定义） | 空 key 是否允许属 API 语义（P2）；M4 不拒绝，按算法定义返回 |

---

## 11. 退出条件（DoD）与验证步骤

1. `util/hash.{h,cpp}` 在 GCC 15 / Clang 20 下编译通过（扩展关闭、`-Wall -Wextra -Wpedantic`、零警告）；`cabe_util` 双工具链构建通过。
2. **已知向量**：`Hash` 对 xxHash 官方测试向量（含空串、定长串）输出与官方一致（证明 vendored 接入正确、未被改写）。
3. **分布卡方**：100K 随机 key 经 `RouteToDevice(key, N)`（如 N=4/8），各桶计数的卡方统计量在自由度 N-1 的合理显著性水平内（分布近似均匀）。
4. **冻结自证**：固定若干 key 的 `Hash` 值写入测试常量，跨工具链/重复构建恒定（守护 D6）。
5. 职责边界：`hash.*` 不出现 CRC 相关调用；`crc32.*` 不出现路由调用（人工/grep 核对）。

> 上述 2–4 的正式用例在 **M5** 固化；M4 阶段可用临时 smoke 自证已知向量与分布，验证后移除。

---

## 12. 对下游里程碑的接口承诺

| 里程碑 | M4 提供的接入点 |
|---|---|
| M5 | `Hash`/`RouteToDevice` 可被单测覆盖（已知向量、分布、跨平台稳定）；bench 可测 hash 吞吐 |
| P2 | 公开 API 的 key 路由语义建立在 `RouteToDevice` 之上（`Options.devices` 的 N） |
| P7 | 两级路由在 `Hash` 之上扩展：`device = Hash%N`、`reactor = (Hash/N)%R`；`Hash` 本身不变 |
| P11 | 多 device 负载均衡测量直接复用 `RouteToDevice` + `Hash` |
| 全期 | `Hash` 输出冻结（D6）：任何阶段不得更改算法/seed/版本，变更等同 v2.0 |
