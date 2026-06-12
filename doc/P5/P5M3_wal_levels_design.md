# Cabe P5-M3 设计：WAL 分级 2/3/4 + 缓冲区配置

> 在 M2 已打通的 WAL 级别 1(最严格)之上，实装级别 2/3/4。延续 M2 的"级别内化"：
> `Engine::Put/Delete` 保持级别无关、基本不动；级别差异全在它俩调用的 `Wal` 与 `IoBackend`
> 内部分支——`Wal` 决定 WAL 同步还是攒批，`IoBackend` 决定 value 要不要 FUA。级别来自
> `Options.wal_level`，由两个组件**每次操作现读**，运行时可改（经 Engine 一个入口）。
>
> M3 仍是单线程 + 同步实现：攒批的"异步"体现在结构(攒缓冲、按触发刷出)上，底层刷盘仍是
> 阻塞 `fdatasync`；真正的后台刷盘线程、定时刷出、双缓冲、零拷贝、io_uring/SPDK 异步全留 P7+。
>
> **本文为详细设计**；C++ 片段为设计示意，代码实装以此为准。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P5 / M3 |
| 状态 | **✅ 已锁定（P5M7 收敛）** |
| 上游依赖 | P5M2（WAL 核心：128 字节帧 + `Wal` 类 + 级别 1 + Engine 集成） |
| 下游依赖本里程碑 | P5M4（快照：检查点时复用 `Flush()` 刷净 WAL）、P5M6（恢复：复用帧 + 双 CRC + `seq`） |
| 退出判定 | 见 §13 |

---

## 1. 目标与范围

### 1.1 目标

1. 在级别 1 之上实装 **WAL 级别 2/3/4**（P5-D4 的四级持久化）。
2. 延续**级别内化**：`Engine` 只分发 I/O，`Wal`/`IoBackend` 各自现读 `Options.wal_level` 分支；`Engine::Put/Delete` 代码不变。
3. **WAL 攒批**（级别 2/4）：`WriteWal` 只追加不每帧刷，攒满 / Close / 切档收紧才刷；新增 `Flush()`。
4. **value 异步**（级别 3/4）：`io.Write` 按级别决定要不要 FUA。
5. **Options 的 WAL 字段生效**：`wal_level`、`wal_buffer_size` 生效；`wal_flush_interval_ms` 存而不用（P7）。
6. **运行时改级别**：Engine 一个改级别入口，收紧时先刷缓冲。

### 1.2 交付范围

1. **`wal/wal.{h,cpp}`**：`Open` 改收 `const Options*`；`WriteWal` 按级别分支（同步 / 攒批）；新增公开 `Flush()`；缓冲按 `wal_buffer_size` 定大小；去掉 `level_` 成员（改为现读 `opts_->wal_level`）。
2. **`io/sync/sync_io_backend.{h,cpp}` + `io/uring/io_uring_backend.{h,cpp}`**：`Open` 改收 `const Options* = nullptr`；`Write` 现读级别决定要不要 `fdatasync`。
3. **`engine/engine.{h,cpp}`**：新增 `Options options_` 成员；`Open` 拷贝 `opts` 并把 `&options_` 传给组件；新增改级别入口（含收紧时 `Flush`）；`Put`/`Delete` 不变。
4. **测试**：扩展 `test/wal/wal_test.cpp`（各级别 WAL 时机 + 三个刷出触发 + 默认级别 3 + 读己之写）。
5. **关联文档**：更新 `doc/P5/README.md` 的 M3 段、核对 `ROADMAP.md`；并按 §12 对全部历史文档做一次对账同步。

### 1.3 推迟范围

| 推迟项 | 落点 | 原因 |
|---|---|---|
| 定时刷出（`wal_flush_interval_ms` 生效） | P7 | 真正的定时刷出要后台线程独立计时；单线程下机会式模拟给不出"空闲也兜底"的承诺，且 P7 会重写，故不做 |
| 双缓冲 + 后台刷（消除 `fsync` 抖动） | P7+ | 抖动真实存在，但消除它要把刷盘挪到独立执行体；单线程同步下双缓冲零收益，只留干净接缝 |
| 零拷贝 / SPDK / 跨后端异步抽象层 | P7+ | 需 io_uring/SPDK 的注册缓冲 / DMA；M3 不焊死特定形态，见 §12 |
| `wal_buffer_size` 运行时改大小 | 未来 Options 维护接口 | 需"先刷 + 重分配"；缓冲大小是次要旋钮，Open 时定死更省 |
| 并发安全的运行时改级别 | P7 | M3 单线程，`SetWalLevel` 与读写同线程顺序发生，无并发问题 |
| 崩溃恢复 / WAL 重放 / value 损坏的恢复策略 | P5M6 | M3 无恢复；级别 3/4 崩溃后的损坏检测靠现有 `Get` CRC，恢复处置在 M6 |
| 环形缓冲 / 回收 | P5M5 | M3 仍线性追加，假定 WAL 不写满 |

---

## 2. 现状盘点（M2 给了什么）

- **`Wal`**：`Open(wal_path, WalLevel level)` 存 `level_` 拷贝；`WriteWal` = `Append` + `Sync`（**每帧**整块写 + `fdatasync`）；持一块 **4K** 缓冲 `cur_buf_`（`kWalBlockSize`）；无 `Flush()`。按 P5M2-D11，**M2 统一按级别 1 运行**，`level_` 已布好线但分支未启用（`WriteWal` 里 `(void)level_;`）。
- **`IoBackend`**（sync / io_uring 两后端）：`Open(path)`；`Write` **无脑 FUA**（`pwrite`/提交 + `fdatasync`）。
- **`Engine`**：`Put`/`Delete` 已接级别 1 WAL（value FUA → WAL 同步 → 内存 → 返回 / 墓碑帧）；**不持有 Options**；`Open` 仅 create 模式开 WAL。
- **`Options`**：`wal_level` 默认 `WalSync`(级别 3)、`wal_buffer_size` 默认 32K、`wal_flush_interval_ms` 默认 1s——**均为占位，M3 起生效**。
- **`wal/wal_frame.h`**：128 字节帧、双 CRC、`seq`，已定，M3 不动。

---

## 3. 关键决策（已锁定）

| 编号 | 决策 | 结论 |
|---|---|---|
| **P5M3-D1** | 级别内化 | `Engine` 只分发 I/O；`Wal`/`IoBackend` 各自**现读** `Options.wal_level` 分支；`Engine::Put/Delete` 代码不变 |
| **P5M3-D2** | Options 归属 | `Engine` 持有 `Options options_`（常驻成员）；`Wal`/`IoBackend` 持 `const Options*` 直接现读；修改走 Engine 一个入口（罕调）；完整 Options 接口集留未来维护 |
| **P5M3-D3** | value 持久（IoBackend） | 级别 1/2 → `Write` 走 FUA；3/4 → 只写不 `fdatasync`。`opts == nullptr`（bench/test 单独构造）默认按**级别 3 行为**（不 FUA），与默认级别一致 |
| **P5M3-D4** | WAL 落盘（Wal） | 级别 1/3 → 同步（每帧落盘）；2/4 → 攒批（攒满/Close/收紧才刷） |
| **P5M3-D5** | 缓冲结构 | **一块**缓冲，大小 = `wal_buffer_size`，Open 时定、一次性分配、运行期固定，Open 时向上取整到 4K 倍数且 ≥4K；两档共用；空闲部分不管；对不完整写入安全（帧 CRC 兜底）。（P5M5 注：攒批"攒满"判定改以**有效窗口** `min(buf_size, 到环尾)` 为准——贴缝截短，见 P5M5 §4） |
| **P5M3-D6** | 刷出触发 + 线程 | 触发 = 攒满 / Close / 切档收紧；**定时器 → P7**；`Flush()` 在调用（engine）线程上同步内联执行 |
| **P5M3-D7** | 抖动与扩展 | 单缓冲 + `Flush()` 干净自包含；双缓冲 / 后台刷 / 零拷贝 / SPDK 全留 P7+，M3 只留干净接缝（`RawDevice` 原语 / 窄 `Flush()` / `AllocAligned` 分配口），不焊死特定形态 |
| **P5M3-D8** | 默认级别 | M3 起按配置级别执行，**默认级别 3**；M2 的"强制级别 1"（P5M2-D11）解除 |
| **P5M3-D9** | 运行时改级别 | 走 Engine 改级别入口；收紧（攒批 2/4 → 同步 1/3）时先 `Flush()`；并发安全 → P7；`wal_buffer_size` 运行时改大小 → 未来 |
| **P5M3-D10** | 读侧 | M3 读路径不加新代码；级别 3/4 崩溃后的 value 损坏由现有 `Get` 的 CRC 兜住；处置策略 → M6 |
| **P5M3-D11** | 改动面 | 改 5 处现有文件；无新文件、无新模块、CMake 不动；M3 不加新错误码（`Flush` 失败复用 `kWalWriteFailed`） |

---

## 4. 四级语义与崩溃一致性

四个级别其实是**两个独立开关**的组合：**value 落盘**（同步 FUA / 异步）、**WAL 落盘**（同步 / 攒批异步）。

| 级别 | value | WAL | 返回时机 | 返回时已持久 |
|---|---|---|---|---|
| 1 Strict | 同步 FUA | 同步 | value + WAL + 内存 | value + WAL |
| 2 ValueSync | 同步 FUA | 攒批 | value + 内存 | value（WAL 在缓冲） |
| 3 WalSync（默认） | 异步 | 同步 | WAL + 内存 | WAL（value 未强制落盘） |
| 4 Async | 异步 | 攒批 | 内存 | 无（都在缓冲） |

**不变量（所有级别）**：先写内存索引再返回（读己之写）。所以**进程不崩**时，Put 完紧接着 Get 一定读得到——级别只影响"崩溃能不能扛住"，不影响进程内可见性。

**崩溃一致性**（恢复是 M6，但级别设计现在就要自洽；下表写进文档）：

| 级别 | 掉电崩溃丢什么 | 失败形态 |
|---|---|---|
| 1 | **不丢**（已确认的写都活） | 无 |
| 2 | 没刷出那批写的 **key 整个消失**（value 落了盘但成孤儿块，恢复时回收） | 干净丢失，**绝不读出损坏** |
| 3（默认） | 最近的 **value 可能损坏**，key 都在 | **键在、值坏**（CRC 查得出 → `kEngineDataCorrupted`） |
| 4 | 没刷出的写整个消失（比级别 2 丢得多） | 干净丢失，绝不读出损坏 |

两个"有意思"的级别，文档要讲清：
- **级别 2**：花了 FUA 把 value 落了盘，但 WAL 攒批——崩溃时这条写的 key 可能随缓冲一起没了。它的价值是"**凡是 WAL 刷出去活下来的写，value 一定是好的**"，永不读出损坏。
- **级别 3（默认）**：反过来——key 永不丢（WAL 同步），但最近 value 可能没落盘，崩溃后读出 CRC 不匹配。这就是"键在值坏"，检测靠现有 `Get` CRC（§9），处置留 M6。

（P5M6 兑现注：本表在恢复设计中两处**真实扣款**——① 级别 2/4 "没刷出的写整个消失"是 M6
撕裂碎片处置（容差 + 从盘上抹除）的正当性来源：碎片帧本就是契约内可丢的帧，抹除等于兑现本表；
"value 成孤儿块恢复时回收"由"空闲 = 终态索引补集"原则落地。② 级别 3 "键在值坏"的处置落定：
重放**保留**该条目（删条目是假装写入没发生），`Get` 时如实报 `kEngineDataCorrupted`；
`verify_value_crc_on_recovery` 开关把发现时刻提前到恢复期（纯诊断、不改恢复成败）。见 P5M6-D13/D15/D16。）

---

## 5. 级别内化架构

```
Engine::Put / Delete         ← 级别无关、代码不变；只调下面两个
  ├─ io.Write(block, buf)    ← IoBackend 现读 wal_level：1/2 FUA，3/4 异步
  └─ wal.WriteWal(entry)     ← Wal 现读 wal_level：1/3 同步，2/4 攒批
                                两者读的都是 Engine 持有的同一份 options_
```

- **`Engine` 持有 `Options options_`**（成员）；`Wal`/`IoBackend` 持 `const Options*` 指向它，每次操作现读。`Engine` 不可移动（`Engine(Engine&&) = delete`），`&options_` 地址稳定；`DeviceContext` 在 `devices_`（vector）里移动也不受影响——指针指向 `Engine` 成员、不在 vector 内。
- **改级别走 Engine 一个入口**（罕调，非每次操作），由它编排切档时的刷缓冲。读级别才是每次操作做的事，在组件内部。
- **性能**：每次 `Write`/`WriteWal` 多一次"读成员 + 比较 + 跳转"的分支，分支预测器几乎全命中（级别极少变），约 1 个时钟周期；后面 gate 的是 1 MiB `pwrite` + `fdatasync`，差约 9 个数量级，可忽略。运行时可变相比 Open 写死，每次操作开销**完全相同**（都查一个 held 标志），只多一个几乎免费的 setter。

---

## 6. `Wal` 改造

### 6.1 接口与状态

```cpp
class Wal {
public:
    // 现读级别：不再传 WalLevel，而是持 const Options*。
    int32_t Open(const std::string& wal_path, const Options* opts);
    int32_t WriteWal(const WalEntry& e);
    int32_t Flush();   // 公开：刷出攒着、未落盘的帧；同步档下是空操作
    int32_t Close();   // 内部先 Flush() 再关
private:
    int32_t Append(const WalEntry& e);

    RawDevice     dev_;
    std::byte*    cur_buf_  = nullptr;   // AllocAligned，大小 = buf_size_
    std::size_t   buf_size_ = 0;         // = 取整后的 wal_buffer_size（Open 定）
    std::uint64_t cur_off_  = 0;         // 当前缓冲窗口在设备上的起始偏移
    std::uint32_t n_frames_ = 0;         // 当前窗口内已写帧数
    std::uint64_t seq_next_ = 1;
    const Options* opts_    = nullptr;   // 现读 wal_level（取代原 level_）
};
```

- `Open`：`dev_.Open` → 读 `opts->wal_buffer_size`，向上取整到 4K 倍数且 ≥4K，`AllocAligned(buf_size_)`、清零 → `cur_off_ = kDataRegionOffset`、`n_frames_ = 0`、`seq_next_ = 1`、`opts_ = opts`。
- 去掉原 `level_` 成员；`WriteWal` 现读 `opts_->wal_level`。

### 6.2 `WriteWal`：按级别分支

```cpp
int32_t Wal::WriteWal(const WalEntry& e) {
    if (e.key.size() > kWalKeyMax) return err::kWalKeyTooLong;
    const WalLevel lvl = opts_->wal_level;        // 现读
    int32_t rc = Append(e);                       // 编码一帧入缓冲、分配 seq、算 CRC
    if (rc != err::kSuccess) return rc;
    if (lvl == WalLevel::Strict || lvl == WalLevel::WalSync) {
        return SyncCurrentBlock();                // 同步档：每帧落盘
    }
    // 攒批档：只追加；缓冲满了才刷
    if (BufferFull()) return Flush();
    return err::kSuccess;
}
```

### 6.3 同步档（级别 1/3）

沿用 M2：用缓冲的**第一个 4K 块**作"当前块"，每写一帧就**重写整块 4K + `fdatasync`**；块满 32 帧 → `cur_off_ += 4K`、`n_frames_ = 0`、清零当前块。缓冲其余部分同步档不用。

### 6.4 攒批档（级别 2/4）

帧**顺序填进整块缓冲**（跨多个 4K 块），只追加不刷；写满整块缓冲 → `Flush()`。

### 6.5 `Flush()`

把缓冲里已用的**整数个 4K 块**一次性 `WriteAt + fdatasync`，`cur_off_` 前移、`n_frames_ = 0`、清零缓冲。**同步档下缓冲没有待刷的帧（每帧已落盘），`Flush()` 是空操作**——所以它在任何档位、任何时刻都能安全调用。

> **P5M5 起本节已超越**：`Flush` 改为"**整块推进、半块留窗**"——写出仍是 `AlignUp(used, 4K)`
> （持久性不变），但 `cur_off_` 只推进整块部分，半块帧留在缓冲头续攒、下次整块同字节重写；
> 提前刷出（Close/切档/快照前）不再留块尾补零空洞。详见 `P5M5_wal_ring_design.md` §6.1。

### 6.6 切档

- **攒批 → 同步（收紧）**：由 Engine 改级别入口在改级别前先调 `Flush()`，保证"同步即落盘"从此刻起成立，缓冲回到干净状态；（P5M5 注：变体 Y 下刷后可**留窗**——`n_frames_ < 32`、帧在缓冲头、`cur_off_` 停在该块,恰为同步档"当前块"的期望状态,切档仍零特殊处理。）
- **同步 → 攒批（放松）**：无需特殊动作——同步档的帧本就已落盘，后续攒批的整块刷会把它们一起重写（同字节、幂等）。

### 6.7 对不完整写入安全

刷出写的是整数个 4K 块、每块都是完整帧。断电刷到一半：没写到的尾块 → 那些帧本就未确认、丢失正确；最多一个被写一半（撕裂）的块 → 帧 `frame_crc` 不过 → 恢复时（M6）当"没写过"。与级别 1 的"重写整块"同一套 CRC 兜底。

---

## 7. `IoBackend` 改造（sync + io_uring 两后端同样处理）

```cpp
int32_t SyncIoBackend::Open(const std::string& path, const Options* opts = nullptr); // 加可选形参

int32_t SyncIoBackend::Write(std::uint64_t block_idx, const std::byte* buf) {
    ... pwrite（含越界守卫、EINTR 重试）...
    // 级别 1/2 → value FUA；3/4 → 异步（不 fdatasync）。
    // opts_ == nullptr（bench/test 单独构造）按级别 3 行为：不 FUA。
    const WalLevel lvl = opts_ ? opts_->wal_level : WalLevel::WalSync;
    if (lvl == WalLevel::Strict || lvl == WalLevel::ValueSync) {
        if (::fdatasync(fd_) < 0) return err::kIoBase;
    }
    return err::kSuccess;
}
```

- `Open` 加可选 `const Options* opts = nullptr`，存 `opts_`；`Write` 现读 `opts_->wal_level` 决定要不要 `fdatasync`（取代 M2 的无脑 FUA）。
- `nullptr` 默认按**级别 3**（不 FUA）——bench/test 单独构造 `IoBackend` 时不关心持久级别，做不做 FUA 都不影响读写往返。
- `IoBackend` 是 concept（`io/io_backend.h`）；`Open` 加默认形参后 `t.Open(path)` 仍可调，concept 仍满足。若 concept 显式约束了 `Open` 形参，则一并微调 `io/io_backend.h`（否则不动）。

---

## 8. `Engine` 改造

### 8.1 持有 Options + 传给组件

```cpp
class Engine {
    ...
    Options options_;                      // 新增成员
    std::vector<DeviceContext> devices_;
};

Status Engine::Open(const Options& opts) {
    ...
    options_ = opts;                       // 先拷贝，地址稳定
    for (...) {
        ...
        dc.io.Open(cfg.data_path, &options_);          // 传指针
        ...
        if (opts.create) dc.wal.Open(cfg.wal_path, &options_);
        ...
    }
}
```

### 8.2 `Put` / `Delete` 不变

确认：`Engine::Put/Delete` 还是固定那套序列（`io.Write` → `wal.WriteWal` → 内存 → 回收旧块 / 墓碑帧），**一行不改**；级别差异全在 `io.Write` 和 `wal.WriteWal` 内部按现读级别分支。

### 8.3 改级别入口

```cpp
// 暂名 SetWalLevel；名字写稿/实装时再定。本质 = 改 options_.wal_level 这个 option。
Status Engine::SetWalLevel(WalLevel new_level) {
    const WalLevel old = options_.wal_level;
    const bool tighten_wal =
        (old == WalLevel::ValueSync || old == WalLevel::Async) &&     // 旧 = WAL 攒批
        (new_level == WalLevel::Strict || new_level == WalLevel::WalSync); // 新 = WAL 同步
    if (tighten_wal) {
        for (auto& dc : devices_) {
            int32_t rc = dc.wal.Flush();   // 收紧前先刷净
            if (rc != err::kSuccess) return Status::Error(rc);
        }
    }
    options_.wal_level = new_level;        // 组件下次操作自然读到
    return Status::Ok();
}
```

- M3 只做这一个最小"改级别"入口；通用 Options 接口、以及对 `wal_buffer_size` 改动的显式拒绝（如 `kEngineOptionImmutable`）留未来维护接口。M3 里 `wal_buffer_size` 没有修改入口，本就改不了。
- 并发安全（多线程下改级别）→ P7。

---

## 9. value 异步的读侧检测（级别 3/4）

- 级别 3/4 的 `io.Write` 只 `pwrite`、不 `fdatasync`。O_DIRECT 下数据可能停在设备易失写缓存，**掉电不保证落到介质**。
- **进程不崩**：value 在缓存/介质上，`Get`（O_DIRECT `pread`）读得到正确字节，CRC 通过——无问题。
- **掉电崩溃 + 恢复后（M6）**：WAL（级别 3 同步）重放出 key→块，但块 value 没落盘 → 块里是旧数据/垃圾 → `Get` 读出 CRC 不匹配 → `kEngineDataCorrupted`。
- **M3 读路径不加任何新代码**：现有 `Get` 的 CRC 校验就是检测路径，不区分"没落盘 / 真损坏"，都返回 `kEngineDataCorrupted`。
- M3 没有恢复、不崩溃，这个场景在 M3 **跑不出来**（要故障注入，已推迟），真正验证在 M6；损坏处置策略（丢弃 / 报上层 / `verify_value_crc_on_recovery`）也在 M6。
- **M3 不主动兜底 value 的最终落盘**（异步档的 value 何时真落到介质，靠 M4 快照检查点 / P7 后台刷），这是异步档"尽力而为"的契约。

---

## 10. 文件改动面 + CMake + 错误码

- **改 5 处现有文件**：`wal/wal.{h,cpp}`、`io/sync/sync_io_backend.{h,cpp}`、`io/uring/io_uring_backend.{h,cpp}`、`engine/engine.{h,cpp}`、`io/io_backend.h`（仅当 concept 约束 `Open` 形参时微调）。
- `engine/options.h`、`engine/device_context.h`：**结构不改**（字段/成员已存在，只是 `Wal`/`IoBackend` 的 `Open` 签名变）。
- **无新文件、无新模块、CMake 不动**。
- **M3 不加新错误码**：`Flush` 失败复用 `kWalWriteFailed`；改级别入口对 `wal_buffer_size` 的显式拒绝码留未来。

---

## 11. 测试设计

前提：M3 单线程同步、不崩溃、无恢复、无故障注入——级别间"崩溃丢什么"测不出来，只能测**可观测的结构性差异**。

| 用例 | 需设备 | 验证点 |
|---|---|---|
| `SyncLevelWritesImmediately` | 是 | 级别 1/3：Put 后用 `RawDevice` 读 WAL 设备 → 帧**立刻在盘** |
| `BatchLevelBuffers` | 是 | 级别 2/4：Put 后读 WAL 设备 → 帧**还不在**；触发刷出后再读 → 帧出现 |
| `FlushOnBufferFull` | 是 | 攒批档连写至攒满 → 刷前不在、刷后在 |
| `FlushOnClose` | 是 | 攒批档写若干 → Close → 帧落盘 |
| `FlushOnTighten` | 是 | 攒批档写若干 → `SetWalLevel` 切到同步档 → 缓冲被刷净 |
| `DefaultLevelIsThree` | 是 | 不设 `wal_level` 打开 → 表现为 WAL 同步（帧立刻在盘） |
| `ReadYourWrites` | 是 | 每个级别下 Put 完紧接 Get 都读回原值 |

- **测不了、推迟**：崩溃下各级别真正丢什么（→ M6 故障注入）；value 的 FUA vs 异步**持久性**（崩溃才显形 → M6 / 代码审查）。
- **落点**：扩展现有 `test/wal/wal_test.cpp`（不动 CMake）。
- 实现细节：攒批档"刷前帧不在盘"的判定，注意 loop 设备可能残留上轮垃圾——测试 setup 时把 WAL 日志区相应位置先清零，避免误判。

---

## 12. 未来演进（写入文档备查，非 M3 实现）

- **定时刷出**：`wal_flush_interval_ms` 生效，需后台线程独立计时 → **P7**。
- **双缓冲 + 后台刷（消抖动）**：单缓冲的 `fsync` 阻塞会造成尾延迟尖刺，真实存在；但消除它要把刷盘挪到独立执行体（后台线程 / 异步 I/O），单线程同步下双缓冲零收益 → **P7+**。
- **跨后端方案**：io_uring（内核、注册缓冲、环收割）与 SPDK（用户态、DMA、轮询、NVMe FUA/Flush）原语层差太远，"普适方案"会退化成最小公约数、丢掉零拷贝。正解是**抽象层**：**上层 WAL 逻辑（帧/级别/攒批/调度/并发/背压）写一次**，**下层 per-backend 实现原语（分配缓冲 / 提交可带持久标志的写 / 收割完成）+ 持久机制**（`fdatasync` / `RWF_DSYNC` / NVMe FUA）。抽象层画在"异步 + 完成 + 后端提供缓冲"这一层，sync 实现成退化版。这与 cabe 现有 `IoBackend`（编译期可换抽象 + per-backend 实现）一脉相承 → **P7+**。
- **`wal_buffer_size` 运行时改大小**：需"先刷 + 重分配"，归未来 Options 维护接口。
- **并发安全的运行时改级别** → P7；**环形缓冲 + 回收** → M5；**崩溃恢复 / 重放 / value 损坏的恢复处置** → M6。
- **M3 留好的干净接缝**（让上述都"换接缝不动上层逻辑"）：WAL I/O 原语走 `RawDevice`；持久收敛到窄接口 `Flush()`；缓冲经 `RawDevice::AllocAligned` 这个分配口拿。这几个接缝不只为 io_uring，而是为整个 io_uring + 零拷贝 + SPDK 的未来留的，所以不焊死 A/B 之类特定形态。

---

## 13. 退出判定

1. `Wal`：`Open(const Options*)`、`WriteWal` 按级别分支（同步 / 攒批）、公开 `Flush()`、缓冲按 `wal_buffer_size` 定大小；去掉 `level_`。
2. `IoBackend`（sync + io_uring）：`Open` 收 `const Options*`、`Write` 现读级别决定 FUA、`nullptr` 按级别 3。
3. `Engine`：持有 `options_`、`Open` 传 `&options_`、新增改级别入口（含收紧 `Flush`）；`Put`/`Delete` 不变。
4. 默认级别 3 生效（M2 的强制级别 1 解除）。
5. `test_wal` 新增用例（WAL 时机 + 三个刷出触发 + 默认级别 3 + 读己之写）全绿；已有测试（M1/M2 等）保持绿色。
6. 关联文档更新（README M3 段、ROADMAP）+ 历史文档按 §12/议题 7 对账同步。
