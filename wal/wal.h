#ifndef CABE_WAL_H
#define CABE_WAL_H

// P5M2/P5M3：WAL 模块的接口与 Wal 类。
// 设计依据：doc/P5/P5M3_wal_levels_design.md §6（在 P5M2 §6 之上分级）。
// 级别内化：Wal 现读 Options.wal_level 分支——
//   级别 1/3（同步）：每帧整块写 + fdatasync；
//   级别 2/4（攒批）：攒进缓冲，攒满 / Close / 切档收紧才 Flush()。
// WAL 复用 RawDevice（不走 IoBackend），由 Wal 持有 WAL 设备生命周期（D6）。

#include "wal/wal_frame.h"
#include "engine/options.h"     // Options / WalLevel
#include "util/raw_device.h"
#include "common/structs.h"     // BlockId

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cabe {

    // 调用方填的逻辑记录。seq 由 Wal 内部分配，frame_crc 由 Wal 内部计算。
    struct WalEntry {
        WalEntryType     type{};        // Put / Delete
        std::string_view key;           // ≤ kWalKeyMax，否则 WriteWal 返回 kWalKeyTooLong
        BlockId          block{};       // Put 有效；Delete 填 {}
        std::uint32_t    value_crc = 0; // Put = value 的 CRC32C；Delete 填 0
        std::uint64_t    timestamp = 0; // 来自 ValueMeta.timestamp（Engine 已算）
    };

    // 把逻辑记录编码成一帧（分配 seq、算 frame_crc）。供 Wal 内部与测试使用。
    WalFrame EncodeFrame(const WalEntry& e, std::uint64_t seq);
    // 校验一帧：魔数 / 版本 / key_len 上限 / 帧 CRC。供测试与 M6 重放使用。
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
        // create 模式：日志从 kDataRegionOffset 起，缓冲清零，seq=1。
        int32_t Open(const std::string& wal_path, const Options* opts);
        // 唯一写入入口：现读 wal_level 分支（同步 / 攒批）。
        int32_t WriteWal(const WalEntry& e);
        // 刷出攒着、未落盘的帧（攒满 / Close / 切档收紧调用）；同步档或缓冲空时为空操作。
        int32_t Flush();
        int32_t Close();

        // P5M4：已分配的最大 seq（= seq_next_-1，空时为 0），供快照取 covered_seq。
        std::uint64_t last_seq() const noexcept { return seq_next_ - 1; }
        // P5M4：WAL 设备容量（部署期校验用；M4 暂不查，见 P5M4 §11 备注，M5 起用）。
        std::uint64_t SizeBytes() const noexcept { return dev_.SizeBytes(); }

    private:
        int32_t Append(const WalEntry& e);     // 编码一帧入缓冲（分配 seq、算 frame_crc）
        int32_t SyncCurrentBlock();            // 同步档：当前 4K 块整块 WriteAt + fdatasync
        bool    sync_level() const noexcept;   // wal_level ∈ {Strict, WalSync}

        RawDevice      dev_;                    // 持有 WAL 设备（不走 IoBackend）
        std::byte*     cur_buf_  = nullptr;     // AllocAligned 缓冲，大小 = buf_size_
        std::size_t    buf_size_ = 0;           // 取整后的 wal_buffer_size（Open 定，4K 倍数）
        std::uint64_t  cur_off_  = 0;           // 当前缓冲窗口在设备上的起始偏移
        std::uint32_t  n_frames_ = 0;           // 当前窗口内已写帧数
        std::uint64_t  seq_next_ = 1;           // 下一帧的 seq
        const Options* opts_     = nullptr;     // 现读 wal_level / wal_buffer_size
    };

} // namespace cabe

#endif // CABE_WAL_H
