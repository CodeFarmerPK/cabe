#ifndef CABE_WAL_H
#define CABE_WAL_H

// P5M2：WAL 模块的接口与 Wal 类。
// 设计依据：doc/P5/P5M2_wal_core_design.md §6。
// 公共只暴露 Open / WriteWal / Close；级别策略封装在 WriteWal 内部（M2 仅级别 1）。
// WAL 复用 RawDevice（不走 IoBackend），由 Wal 持有 WAL 设备生命周期（D6）。

#include "wal/wal_frame.h"
#include "engine/options.h"     // WalLevel
#include "util/raw_device.h"
#include "common/structs.h"     // BlockId

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
    // 要求 e.key.size() ≤ kWalKeyMax（调用方保证；WriteWal 已先行校验）。
    WalFrame EncodeFrame(const WalEntry& e, std::uint64_t seq);

    // 校验一帧：魔数 / 版本 / key_len 上限 / 帧 CRC。供测试与 M5 重放使用。
    bool VerifyFrame(const WalFrame& f);

    class Wal {
    public:
        Wal() noexcept = default;
        ~Wal();

        Wal(const Wal&) = delete;
        Wal& operator=(const Wal&) = delete;
        Wal(Wal&& other) noexcept;            // DeviceContext 经 std::move 入 vector
        Wal& operator=(Wal&& other) noexcept; // 移动时置空源 buf_/dev_，避免双重释放

        // 打开 WAL 设备并持有级别。create 模式：日志从 kDataRegionOffset 起，当前块清零，seq=1。
        int32_t Open(const std::string& wal_path, WalLevel level);
        // 唯一写入入口：内部按级别决定落盘/返回时机（M2 统一按级别 1 = 同步落盘）。
        int32_t WriteWal(const WalEntry& e);
        int32_t Close();

    private:
        int32_t Append(const WalEntry& e);   // 编码一帧入当前块缓冲（分配 seq、算 frame_crc）
        int32_t Sync();                       // 当前块整块 WriteAt + fdatasync

        RawDevice     dev_;                   // 持有 WAL 设备（不走 IoBackend）
        std::byte*    cur_buf_   = nullptr;   // AllocAligned 的 4K 当前块缓冲
        std::uint64_t cur_off_   = 0;         // 当前块在设备上的偏移
        std::uint32_t slot_      = 0;         // 当前块内已用帧数 0..kWalFramesPerBlock
        std::uint64_t seq_next_  = 1;         // 下一帧的 seq
        WalLevel      level_     = WalLevel::Strict; // M2 不区分；M3 起据此分支
    };

} // namespace cabe

#endif // CABE_WAL_H
