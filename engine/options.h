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

    // 四档 = 两个正交开关（P5-D4）：WAL 是否同步落盘、value 是否 FUA。集中在此，
    // 供 Wal / IoBackend / Engine 现读级别时统一判定，避免散落硬编码枚举集合。
    inline constexpr bool IsWalSyncLevel(WalLevel lvl) noexcept {   // WAL 每帧同步落盘（1/3）
        return lvl == WalLevel::Strict || lvl == WalLevel::WalSync;
    }
    inline constexpr bool IsValueFuaLevel(WalLevel lvl) noexcept {  // value FUA 持久（1/2）
        return lvl == WalLevel::Strict || lvl == WalLevel::ValueSync;
    }
    inline constexpr bool IsValidWalLevel(WalLevel lvl) noexcept {  // 枚举合法性（1..4）
        return lvl == WalLevel::Strict || lvl == WalLevel::ValueSync ||
               lvl == WalLevel::WalSync || lvl == WalLevel::Async;
    }

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
        std::size_t wal_buffer_size = 32 * 1024;                // 同步/攒批共用的单块缓冲，默认 32K；Open 时定死、运行期固定（运行时改大小留未来 Options 维护接口）
        std::uint32_t wal_flush_interval_ms = 1000;             // 定时刷出兜底，默认 1s；M3 不读此字段（攒满/Close/切档刷），定时刷出需后台线程，推迟 P7

        // ---- 快照配置（全局统一）----
        std::uint64_t snapshot_threshold_bytes = 512ull * 1024 * 1024; // WAL 增长达此值触发快照（M4 生效），默认 512M
        std::size_t   snapshot_buffer_size     = 1024 * 1024;          // 快照流式写的临时缓冲（M4 生效，可动态改）；默认 1M，每次快照现读、向上取整 4K
        std::uint32_t snapshot_interval_sec    = 600;                  // 定时快照兜底（P7 生效，M4 不读），默认 10 分钟；触发 = 大小阈值 OR 定时

        // ---- 恢复配置（M6 起生效，M1 占位）----
        bool verify_value_crc_on_recovery = false;             // 恢复时是否逐个校验 value CRC，默认关
    };

} // namespace cabe

#endif // CABE_OPTIONS_H
