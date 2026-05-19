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

        // io_uring 后端专用:Submission Queue 深度(P4 M6 / D7 第二部分)。
        //
        // 约束(Open 时校验,违反返回 InvalidArgument):
        //   - 必须是 2 的幂(io_uring_queue_init 旧内核硬性要求;统一锁紧
        //     约束便于跨版本兼容)
        //   - 必须 >= buffer_pool_count(R12;M7 batch API 上线后,一次
        //     可同时 in-flight 的 op 上限是 buffer_pool_count,因此 SQ depth
        //     必须容得下)
        //
        // sync 后端忽略此字段(SyncIoBackend::Open 接收同样参数但不使用,
        // 保持 IoBackendTraits 接口一致)。
        //
        // 默认 64:Model A 一次只发 1 个 op,depth 数值不影响当前 M5 性能;
        // M7 batch 起决定单次 batch 可同时 in-flight 上限,届时按设备
        // 队列深度调整,典型 64-256。
        uint32_t io_uring_sq_depth = 64;
        // ============================================================
        // FreeList 改造可调参数(P4.5 M4 / 设计稿 D-3 / D-6 / D-7 / D-NEW-1)
        //
        // Engine::Open 入口严格校验(违反返回 InvalidArgument);经内部
        // Engine::SetFreeListTuning 透传到 FreeList::SetTuning(后者对
        // 越界值静默忽略,严格校验在 Engine::Open 分层做)。
        // ============================================================

        // freeList 已用比例触发切换(默认 0.90,即剩余 ≤ 10% 触发)。
        // 取值范围 (0, 1)。
        double freelist_switch_ratio = 0.90;

        // 全局已用比例触发写保护(默认 0.90,即全局可用 ≤ 10% 时
        // Allocate 返回 ResourceExhausted)。取值范围 (0, 1)。
        double freelist_reject_ratio = 0.90;

        // 对称水位触发倍数(默认 1.5,active ≥ freeList × 1.5 触发切换)。
        // 取值范围 > 0。
        double freelist_symmetric_ratio = 1.5;

        // 切换前 active 最小 BlockId 数(默认 1024,化解启动后纯写入
        // 导致 freeList 变空的死锁风险 R-NEW-1)。取值范围 ≥ 1。
        uint64_t freelist_min_recycle_threshold = 1024;
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
