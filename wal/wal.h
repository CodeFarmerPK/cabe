#ifndef CABE_WAL_H
#define CABE_WAL_H

// P5M2/P5M3/P5M5/P5M6：WAL 模块的接口与 Wal 类。
// 设计依据：doc/P5/P5M5_wal_ring_design.md（环形化）+ P5M6_recovery_design.md §6/§8（恢复）。
// 级别内化：Wal 现读 Options.wal_level 分支——
//   级别 1/3（同步）：每帧整块写 + fdatasync；
//   级别 2/4（攒批）：攒进缓冲，攒满 / Close / 切档收紧才 Flush()。
// P5M5 环形化：日志区 [ring_start, ring_end) 模环推进、到尾绕回；头尾指针只活内存
//   （盘上真相 = 帧 + 快照槽头 covered_seq）；快照成功后经 ReclaimUpTo 回收（head 跳跃前进）；
//   空间不足时 WriteWal 返回 kWalFull（Engine 层做撞墙救援）。四条不变量见设计稿 §11。
// P5M6 恢复：Open 公共化（两模式共享几何 + 容量校验；recover 缓冲推迟到 Recover 分配——
//   兼作扫描缓冲 + 既有空缓冲守卫天然拒绝过早写入）；Recover = census 普查 → 走读投递
//   （单判据"下一槽 = 下一 seq"）→ 余环清扫（碎片双条件容差）→ 碎片抹除（恢复链唯一写动作）
//   → 运行态重建（锚帧块内续写 + 缓冲重灌 + 稠密续号）→ 收尾自检。
// WAL 复用 RawDevice（不走 IoBackend），由 Wal 持有 WAL 设备生命周期（D6）。

#include "wal/wal_frame.h"
#include "engine/options.h"     // Options / WalLevel
#include "util/raw_device.h"
#include "common/structs.h"     // BlockId / kDataRegionOffset

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

namespace cabe {

    // 由 WAL 设备字节数算环容量：可用区（8K 超级块之后）向下取整 4K（P5M5-D1）。
    // "向下取整"身兼两职：环内一切推进后偏移仍 4K 对齐 + 永不冲出设备尾；零头留空。
    // 环几何的单一来源——Wal::Open 与测试共用，杜绝复抄漂移（对标快照侧 SnapshotSlotSize 先例）。
    inline constexpr std::uint64_t WalRingSize(std::uint64_t device_bytes) noexcept {
        return device_bytes <= kDataRegionOffset
                   ? 0
                   : ((device_bytes - kDataRegionOffset) / kWalBlockSize) * kWalBlockSize;
    }

    // 调用方填的逻辑记录。seq 由 Wal 内部分配，frame_crc 由 Wal 内部计算。
    struct WalEntry {
        WalEntryType     type{};        // Put / Delete
        std::string_view key;           // ≤ kWalKeyMax，否则 WriteWal 返回 kWalKeyTooLong
        BlockId          block{};       // Put 有效；Delete 填 {}
        std::uint32_t    value_crc = 0; // Put = value 的 CRC32C；Delete 填 0
        std::uint64_t    timestamp = 0; // 来自 ValueMeta.timestamp（Engine 已算）
    };

    // P5M6：重放投递回调——写侧递 WalEntry 进、重放还 WalEntry 出（对称镜像，零新类型）。
    // Wal 只做机械解码（entry_type 原样透传不解释，语义解释收口 Engine）；
    // e.key 视图指向扫描缓冲，仅回调期间有效；回调返非成功则走读立停、错误原样上抛。
    using WalReplayFn = std::function<int32_t(const WalEntry& e, std::uint64_t seq)>;

    // 把逻辑记录编码成一帧（分配 seq、算 frame_crc）。供 Wal 内部与测试使用。
    WalFrame EncodeFrame(const WalEntry& e, std::uint64_t seq);
    // 校验一帧：魔数 / 版本 / key_len 上限 / 帧 CRC。恢复扫描的逐槽判据（P5M6 兑现）。
    bool VerifyFrame(const WalFrame& f);

    class Wal {
    public:
        Wal() noexcept = default;
        ~Wal();

        Wal(const Wal&) = delete;
        Wal& operator=(const Wal&) = delete;
        Wal(Wal&& other) noexcept;            // DeviceContext 经 std::move 入 vector
        Wal& operator=(Wal&& other) noexcept; // 移动时置空源 buf_/dev_，避免双重释放

        // 打开 WAL 设备并持有 Options 指针（现读 wal_level；Open 时读 wal_buffer_size 定缓冲大小）。
        // 两模式公共段：算环几何 + 部署期容量校验 ring_size ≥ max(阈值×2, 缓冲+4K)，不足返回
        // kDeviceTooSmall（create/recover 一视同仁，P5M6-D5 兑现 M5-D16 的 recover 欠账）。
        // create（opts->create）：分配缓冲 + 空环（head == tail == ring_start），seq=1。
        // recover：到此为止——缓冲不分配（cur_buf_ 空 = 既有写守卫天然拒绝过早写入），
        //          环状态待 Recover() 从盘上重建；Engine 编排保证 Open 后先 Recover 再写。
        int32_t Open(const std::string& wal_path, const Options* opts);

        // P5M6：恢复（仅 recover 模式、Open 之后调一次）。covered_seq 来自快照裁决（Engine 转手）。
        //   census 全环普查（O(1) 内存）→ 门槛裁决（min_live == covered+1，历史无缺页）
        //   → 走读按 seq 序投递活帧（"下一槽 = 下一 seq"单判据，同时盘上验收不变量③④）
        //   → 余环清扫（集外活帧过碎片双条件容差，越界即拒）→ 碎片抹除（有界、幂等、仅有碎片时）
        //   → 运行态重建（cur_off/head/n_frames/缓冲重灌/seq_next=锚帧+1/窗口）→ 收尾自检。
        // 失败语义：证据矛盾 kWalRecoveryCorrupted / 读错 kWalRecoveryReadFailed /
        //   自检不过 kWalRecoveryInvariant（FATAL）/ 抹除写失败 kWalWriteFailed；
        //   任何失败后本对象只可 Close（Engine 走 AbortOpen）。
        int32_t Recover(std::uint64_t covered_seq, const WalReplayFn& fn);

        // 唯一写入入口：现读 wal_level 分支（同步 / 攒批）。
        // P5M5：环空间不足返回 kWalFull（被拒帧未编码、seq 未耗——重试即完整重走）；
        // 满态下惰性重开窗（回收后下一次写自然复活）。
        int32_t WriteWal(const WalEntry& e);
        // 刷出攒着、未落盘的帧（攒满 / Close / 切档收紧 / 快照前调用）；同步档或缓冲空时为空操作。
        // P5M5 变体 Y："整块推进、半块留窗"——提前刷出不留块尾空洞，半块帧留缓冲头续攒、
        // 下次整块同字节重写（M2 撕裂安全模式）；活区间内帧紧凑连续（不变量③）。
        int32_t Flush();
        int32_t Close();

        // P5M4：已分配的最大 seq（= seq_next_-1，空时为 0），供快照取 covered_seq。
        std::uint64_t last_seq() const noexcept { return seq_next_ - 1; }
        // P5M4：WAL 设备容量（P5M5 起用于 Open 容量校验；亦供测试取几何）。
        std::uint64_t SizeBytes() const noexcept { return dev_.SizeBytes(); }

        // ---- P5M5：回收三口（设计稿 §7）----
        // 捕获口：当前回收边界候选（= 窗口起点）。仅在 DoSnapshot 的定格时刻（Flush 之后）
        // 与 last_seq() 成对捕获才有回收语义——窗口起点之前的帧全部 ≤ covered_seq 且永不再被
        // 重写（变体 Y 只重写留窗块）。本身只是读数，Wal 不认识快照。
        std::uint64_t reclaim_boundary() const noexcept { return cur_off_; }
        // 回收口：head 跳到 boundary（纯内存一行赋值；铁律靠调用时机落实——仅快照成功后调）。
        // 三条几何校验（4K 对齐 / 环内 / [head,tail] 模环路径），不过则 head 不动、记 FATAL、
        // 返回 kWalInvalidReclaim（保守失败：空间暂不复用，正确性零损，下次快照自然再收）。
        int32_t ReclaimUpTo(std::uint64_t boundary);
        // 观测口：回收头（测试断言 / 诊断用，纯读）。
        std::uint64_t head_off() const noexcept { return head_off_; }

    private:
        struct RecoverCensus;                  // P5M6：census 五标量（定义在 wal.cpp）
        int32_t Append(const WalEntry& e);     // 编码一帧入缓冲（分配 seq、算 frame_crc）
        int32_t SyncCurrentBlock();            // 同步档：当前 4K 块整块 WriteAt + fdatasync
        bool    sync_level() const noexcept;   // wal_level ∈ {Strict, WalSync}
        // 在 pos（4K 对齐、环内）开窗的容量：min(缓冲, 到环尾, 准入余量)——跨缝截短（D3）+
        // 恒留一块准入（D5/D12）三合一；三个量都是 4K 倍数故结果天然对齐。0 = 环满。
        std::uint64_t WindowAt(std::uint64_t pos) const noexcept;
        // P5M6：第一遍普查——线性读全环、逐槽三分类（活/残/无效）、只采五标量不裁决。
        int32_t CensusScan(std::uint64_t covered_seq, RecoverCensus* out);
        // P5M5-D15：TRIM 空桩（回收成功后调；实施归 P7）。
        void TrimReclaimedRange(std::uint64_t old_head, std::uint64_t new_head);

        RawDevice      dev_;                    // 持有 WAL 设备（不走 IoBackend）
        std::byte*     cur_buf_  = nullptr;     // AllocAligned 缓冲，大小 = buf_size_
        std::size_t    buf_size_ = 0;           // 取整后的 wal_buffer_size（Open 定，4K 倍数）
        std::uint64_t  cur_off_  = 0;           // 写尾 tail：当前窗口/当前块在设备上的起始偏移
        std::uint32_t  n_frames_ = 0;           // 当前窗口内已写帧数
        std::uint64_t  seq_next_ = 1;           // 下一帧的 seq（绕圈不重置，消歧靠它）
        const Options* opts_     = nullptr;     // 现读 wal_level / wal_buffer_size
        // ---- P5M5 环形状态（头尾指针只活内存，盘上真相 = 帧 + covered_seq；恢复凭扫描推回）----
        std::uint64_t  ring_start_ = 0;         // 环起点（= kDataRegionOffset；Open 后只读）
        std::uint64_t  ring_end_   = 0;         // 环终点（开区间端点；Open 后只读）
        std::uint64_t  head_off_   = 0;         // 回收头：head 之前（模环）已回收可复用；仅 ReclaimUpTo 动
        std::size_t    window_bytes_ = 0;       // 当前窗口有效容量（开窗时算；0 = 环满，写入惰性重开）
    };

} // namespace cabe

#endif // CABE_WAL_H
