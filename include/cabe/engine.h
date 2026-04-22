/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 */

#pragma once

#include "cabe/options.h"
#include "cabe/status.h"
#include <cstddef>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace cabe {

    // Cabe 存储引擎的公开 API 入口。
    //
    // 定位：嵌入式存储引擎（类 LevelDB），以静态库形式链接进调用方进程，
    //       C++ API 是产品本身。调用方通过本类读写数据，引擎负责
    //       "数据如何在 NVMe 上高效存储和检索"，其余逻辑由调用方决定。
    //
    // 基本用法：
    //   cabe::Options opts;
    //   opts.path = "/var/data/cabe.db";
    //   std::unique_ptr<cabe::Engine> db;
    //   cabe::Status s = cabe::Engine::Open(opts, &db);
    //   if (!s.ok()) { /* 处理错误 */ }
    //
    //   auto data = std::vector<std::byte>{...};
    //   s = db->Put({}, "photo.jpg", data);
    //
    //   std::vector<std::byte> out;
    //   s = db->Get({}, "photo.jpg", &out);
    //
    // 线程安全：
    //   - 多线程并发 Get / Size / IsOpen 安全（内部 shared_lock）
    //   - Put / Delete / Close 与任何其他操作互斥（内部 unique_lock）
    //   - P3 前 Put / Get 锁持有时间 ≈ 单次 I/O 时间（毫秒级），不适合高并发写
    //   - P3（io_uring）引入后，I/O 阶段将脱离锁保护，锁持有时间大幅缩短
    //
    // 生命周期：
    //   - Open 工厂方法是唯一合法构造路径（构造函数私有）
    //   - 析构自动调用 Close；需检查关闭错误时请显式调用 Close()
    //   - **析构并发约束**：调用 ~Engine 时必须保证无其他线程正在调用本对象任何方法。
    //     析构内部不能防御此 race（mutex_ 自身正在销毁），违反此约束行为未定义。
    //     长生命周期建议把 Engine 用 std::shared_ptr 持有，所有调用方释放后再析构。
    //
    // 多实例约束（P3 前）：
    //   - 同一 path 同时只能存在一个 Engine 实例（多进程或同进程多实例）。
    //   - 当前未实现 LOCK 文件保护，并发 Open 同一 path 会导致互相截断 / 数据损坏。
    //   - P3 计划引入 LOCK 文件 + flock，届时该约束可放宽为"跨进程互斥"。
    //
    // 隔离原则（Pimpl）：
    //   本头文件不引入任何内部头文件（MetaIndex / ChunkIndex / BufferPool 等），
    //   调用方仅依赖标准库和 cabe/ 公开头文件，内部实现变更不触发调用方重编译。
    class Engine {
    public:
        // 打开（或创建）数据库。
        //
        // 成功：*result 获得 Engine 的独占所有权，返回 Status::OK()。
        // 失败：*result 保持不变（通常为 nullptr），返回对应错误：
        //   - InvalidArgument：result 为 nullptr、opts.path 为空、buffer_pool_count 为 0、
        //                      error_if_exists=true 且路径已存在
        //   - NotFound：create_if_missing=false 且路径不存在
        //   - IOError：文件创建失败、预分配空间失败、内部 Open 失败
        [[nodiscard]] static Status Open(const Options& opts, std::unique_ptr<Engine>* result);

        ~Engine();

        // Engine 持有独占的磁盘资源（fd / mmap），禁止拷贝和移动。
        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        // 写入键值对。
        //
        // value 大小不受限，引擎自动按 1 MiB 切分 chunk 存储。
        // 覆盖写语义：若 key 已存在（含已 Delete 状态），旧数据及其占用的
        //             磁盘块将被原子替换并回收，不会产生空间泄漏。
        //
        // 错误：
        //   - InvalidArgument：key 为空 或 value 为空
        //   - IOError：磁盘写入失败
        //   - ResourceExhausted：BufferPool 耗尽或磁盘空间不足
        [[nodiscard]] Status Put(const WriteOptions& opts, std::string_view key, std::span<const std::byte> value);

        // 便捷重载：使用默认 WriteOptions{}，避免调用方写 `Put({}, key, value)` 的噪音。
        [[nodiscard]] Status Put(const std::string_view key, const std::span<const std::byte> value) {
            return Put(WriteOptions{}, key, value);
        }

        // 读取键值对，将完整 value 写入 *value（内部自动合并多 chunk 并校验 CRC）。
        //
        // 成功时 *value 被 resize 并填充；失败时 *value 被清空(clear())。
        //
        // 错误：
        //   - InvalidArgument：key 为空 或 value 为 nullptr
        //   - NotFound：key 不存在或已被 Delete
        //   - Corruption：CRC 校验失败（数据损坏）
        //   - IOError：磁盘读取失败
        [[nodiscard]] Status Get(const ReadOptions& opts, std::string_view key, std::vector<std::byte>* value);

        // 便捷重载：使用默认 ReadOptions{}。
        [[nodiscard]] Status Get(const std::string_view key, std::vector<std::byte>* value) {
            return Get(ReadOptions{}, key, value);
        }

        // 删除键值对（单次原子操作：逻辑删除 + 物理移除 + 磁盘块回收）。
        //
        // 与 LevelDB 的 Delete 语义对齐：调用方无需感知"逻辑删除"和"物理删除"
        // 的内部分阶段实现，一次 Delete 调用即完成所有清理。
        //
        // 错误：
        //   - InvalidArgument：key 为空
        //   - NotFound：key 不存在或已删除
        [[nodiscard]] Status Delete(const WriteOptions& opts, std::string_view key);

        // 便捷重载：使用默认 WriteOptions{}。
        [[nodiscard]] Status Delete(const std::string_view key) {
            return Delete(WriteOptions{}, key);
        }
        // 返回当前处于 Active 状态的 key 数量。
        // 已 Delete 的 key 不计入；覆盖写不改变计数。
        [[nodiscard]] size_t Size() const;

        // 返回 Engine 是否处于可服务状态。
        [[nodiscard]] bool IsOpen() const;

        // 显式关闭：刷新所有内部状态、释放 BufferPool、关闭文件描述符。
        // 析构函数会自动调用（忽略错误）；需确认关闭成功时请显式调用本方法。
        Status Close();

    private:
        Engine() = default; // 仅通过 Open 工厂方法构造

        struct Impl; // 前向声明，定义在 engine_api.cpp
        std::unique_ptr<Impl> impl_; // Pimpl：内部实现不泄漏到公开头文件
    };

} // namespace cabe
