#ifndef CABE_OPTIONS_H
#define CABE_OPTIONS_H

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace cabe {

    // WAL 持久化级别（P5-D4）。性能递增、可靠性递减。
    enum class WalLevel : std::uint8_t {
        Strict    = 1,  // value 落盘 + WAL 落盘 + 内存 → 返回（最严格，崩溃不丢）
        ValueSync = 2,  // value 落盘 + 内存 → 返回（WAL 攒批异步）
        WalSync   = 3,  // WAL 落盘 + 内存 → 返回（value 异步）—— 默认
        Async     = 4,  // 仅内存 → 返回（value + WAL 全异步）
    };

    // 设备组：每个数据设备关联一块 WAL 设备 + 一块快照设备（P5-D3/D6）。
    // 注意：三成员同为 string，位置式聚合初始化 {a,b,c} 易把 wal/snapshot 写反且无编译报错；
    // 构造时建议用逐字段赋值（.data_path=.../.wal_path=...）或确保顺序为 数据→WAL→快照。
    struct DeviceConfig {
        std::string data_path;       // 数据设备（裸块设备，存 value）
        std::string wal_path;        // WAL 设备（裸块设备，存元数据日志）
        std::string snapshot_path;   // 快照设备（裸块设备，存索引镜像）
    };

    struct Options {
        std::vector<DeviceConfig> devices;   // 设备组列表，N = size()

        // 启动模式（P5-D4）：false=recover（默认，打开已有实例）；true=create（破坏性初始化）
        bool create = false;

        // ---- WAL 配置（全局统一；M3 起生效，M1 占位）----
        WalLevel wal_level = WalLevel::WalSync;                 // 默认级别 3
        std::size_t wal_buffer_size = 32 * 1024;                // 攒批缓冲，默认 32K（仅级别 2/4；运行时可调）
        std::uint32_t wal_flush_interval_ms = 1000;             // 级别 4 定时刷出兜底，默认 1s

        // ---- 快照配置（全局统一；M4 起生效，M1 占位）----
        std::uint64_t snapshot_threshold_bytes = 512ull * 1024 * 1024; // WAL 达此值触发快照，默认 512M
        std::uint32_t snapshot_interval_sec = 600;              // 定时快照兜底，默认 10 分钟
                                                                // 触发 = 大小阈值 OR 定时，任一满足

        // ---- 恢复配置（M5 起生效，M1 占位）----
        bool verify_value_crc_on_recovery = false;             // 恢复时是否逐个校验 value CRC，默认关
    };

} // namespace cabe

#endif // CABE_OPTIONS_H
