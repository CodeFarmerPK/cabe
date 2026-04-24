/*
* Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 */

#pragma once

#include <cstdint>
#include <string>

namespace cabe {

    // Engine::Open 的全局配置。每个 Engine 实例创建时一次性指定，之后不可修改。
    //
    // 裸设备语义(Cabe 设计定位):
    //   Cabe 是直接操作裸块设备的存储引擎,不接受普通文件作为 backing。
    //   Engine 不会创建、不会 truncate、不会 unlink device_path——这三个操作
    //   对块设备节点都是危险或无意义的。devicePath 必须由调用方(或 sysadmin /
    //   scripts/mkloop.sh)预先准备好。
    struct Options {
        // 块设备路径(必填,不能为空)。
        //
        // 必须是已存在的块设备节点,典型值:
        //   - 生产: /dev/nvme0n1, /dev/sda, /dev/disk/by-id/...
        //   - 开发: /dev/loopX (用 scripts/mkloop.sh 创建)
        //
        // Storage::Open 会通过 fstat + S_ISBLK 校验,不是块设备返回
        // Status::InvalidArgument(内部码 DEVICE_NOT_BLOCK_DEVICE)。
        // 通过 ioctl(BLKGETSIZE64) 取设备容量,过小返回 DEVICE_TOO_SMALL。
        std::string device_path;

        // BufferPool 中的 1 MiB 对齐缓冲区数量。
        // 影响同时可进行的 I/O 操作数量。
        // P3(io_uring)引入后,此值决定异步批量 I/O 的并行深度上限。
        uint32_t buffer_pool_count = 8;
    };

    // 每次 Get 操作的选项。
    // 当前字段为预留扩展点,P3+ 逐步启用。
    struct ReadOptions {
        // CRC32C 校验开关。
        // 当前 Engine 始终校验 CRC,此字段为 P3+ 提供关闭校验的扩展点。
        bool verify_checksums = true;

        // P5+ 预留:快照读
        // const Snapshot* snapshot = nullptr;
    };

    // 每次写操作(Put / Delete)的选项。
    // 当前为空,P3 引入 io_uring 异步写时此处将扩展 completion callback 等字段。
    struct WriteOptions {
        // 当前 Cabe 使用 O_SYNC,所有写入强制持久化,此处暂无可配置项。
        // P3 预留:
        // std::function<void(Status)> on_complete;
    };

} // namespace cabe
