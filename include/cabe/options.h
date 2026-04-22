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
    struct Options {
        // 数据文件路径（必填）。
        // 可以是普通文件（开发/测试场景）或块设备路径（生产场景，如 /dev/nvme0n1）。
        std::string path;

        // true：path 不存在时自动创建并预分配 initial_file_size 字节（仅对普通文件有效）。
        // false：path 不存在则返回 Status::NotFound。
        bool create_if_missing = true;

        // true：path 已存在则返回 Status::InvalidArgument（用于确保全新数据库）。
        // false：path 已存在则直接打开。
        bool error_if_exists = false;

        // BufferPool 中的 1 MiB 对齐缓冲区数量。
        // 影响同时可进行的 I/O 操作数量。
        // P3（io_uring）引入后，此值决定异步批量 I/O 的并行深度上限。
        uint32_t buffer_pool_count = 8;

        // create_if_missing = true 时新建文件的预分配大小（字节）。
        // 对块设备无效（块设备容量由硬件决定，跳过 ftruncate）。
        // 默认 64 MiB（可容纳 64 个 1 MiB chunk），生产场景应按预期数据量调整。
        uint64_t initial_file_size = 64ULL * 1024 * 1024;
    };

    // 每次 Get 操作的选项。
    // 当前字段为预留扩展点，P3+ 逐步启用。
    struct ReadOptions {
        // CRC32C 校验开关。
        // 当前 Engine 始终校验 CRC，此字段为 P3+ 提供关闭校验的扩展点。
        bool verify_checksums = true;

        // P5+ 预留：快照读
        // const Snapshot* snapshot = nullptr;
    };

    // 每次写操作（Put / Delete）的选项。
    // 当前为空，P3 引入 io_uring 异步写时此处将扩展 completion callback 等字段。
    struct WriteOptions {
        // 当前 Cabe 使用 O_SYNC，所有写入强制持久化，此处暂无可配置项。
        // P3 预留：
        // std::function<void(Status)> on_complete;
    };

} // namespace cabe
