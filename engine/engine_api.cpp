/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 *
 * cabe::Engine 的实现——P2 公开 API 层。
 *
 * 职责：
 *   1. 类型适配：string_view / span<const byte> → string / DataView（内部类型）
 *   2. 错误翻译:int32_t 内部错误码 → cabe::Status(调用方可感知的语义分类)
 *   3. 错误日志:翻译前按严重程度记录内部码(内部码不泄漏给调用方)
 *
 * 裸设备语义:
 *   早期版本曾在 Open 中通过 ::stat + ::open(O_CREAT|O_TRUNC) + ::ftruncate
 *   做"文件不存在则创建"的语义,失败时还会 ::unlink 路径——这些操作对
 *   裸块设备节点都是危险或无意义的(unlink 删除 udev 节点;O_TRUNC 在
 *   部分内核路径上会清前缀)。本文件已删除全部文件创建/截断/删除路径,
 *   严格执行"backing 必须是已存在的块设备节点"语义,backing 生命周期
 *   由调用方(sysadmin / scripts/mkloop.sh)管理。
 *
 * 边界原则：
 *   - 内部错误码(CHUNK_NOT_FOUND、FREE_LIST_DOUBLE_RELEASE 等)止步于 TranslateStatus
 *   - 公开 Status 的消息字符串面向调用方,不含内部实现细节
 */

#include "cabe/engine.h"
#include "common/error_code.h"
#include "common/logger.h"
#include "common/structs.h"
#include "engine/engine.h"

namespace cabe {

    // ============================================================
    // Impl：Pimpl 实现体，持有内部 Engine 实例
    //
    // cabe::Engine 的公开头文件中只有 `struct Impl;` 的前向声明，
    // 内部 Engine 类的头文件（及其所有传递依赖）对调用方完全不可见。
    // ============================================================
    struct Engine::Impl {
        ::Engine engine;
    };

    // ============================================================
    // 内部工具：错误码翻译
    //
    // 错误码 → Status 的映射规则集中于此，不散落在各方法中。
    // 调用方在调用 TranslateStatus 之前先使用 CABE_LOG_* 记录内部码。
    //
    // 日志级别约定：
    //   DEBUG — 正常业务路径（NotFound 是预期分支，不是错误）
    //   WARN  — 调用方编程错误（空 key 等）
    //   ERROR — 系统级故障（I/O 失败、内存耗尽）
    //   FATAL — 内部不变式被破坏（double-release 等，代表引擎内部 bug）
    // ============================================================
    static Status TranslateStatus(const int32_t code) noexcept {
        switch (code) {
        case SUCCESS:
            return Status::OK();

        // 真正的"key 不存在或逻辑已删除"——只来自 MetaIndex（第一层）。
        // CHUNK_NOT_FOUND / CHUNK_DELETED 来自 ChunkIndex（第二层），意味着
        // 第一层"key 存在"但第二层"chunk 不存在"——两层不一致，属 Corruption。
        case INDEX_KEY_NOT_FOUND:
        case INDEX_KEY_DELETED:
            return Status::NotFound();

        // 参数非法：空 key、nullptr、尺寸违规等
        // 注意：MEMORY_EMPTY_KEY / MEMORY_EMPTY_VALUE 当前路径未使用，但 error_code.h
        // 已 reserve；linter 多次将这两条 case 误删——请勿删除，确保 P4+ 引入这些
        // 内部码后能立即正确映射。
        case CABE_EMPTY_KEY:
        case CABE_EMPTY_VALUE:
        case CABE_INVALID_DATA_SIZE:
        case MEMORY_NULL_POINTER_EXCEPTION:
        case MEMORY_EMPTY_KEY: // reserved by error_code.h，防御性映射
        case MEMORY_EMPTY_VALUE: // reserved by error_code.h，防御性映射
        case POOL_INVALID_PARAMS:
        case POOL_ALREADY_INITIALIZED:
            return Status::InvalidArgument();

        // 设备 I/O 失败
        case DEVICE_FAILED_TO_OPEN_DEVICE:
        case DEVICE_FAILED_TO_CLOSE_DEVICE:
        case DEVICE_FAILED_TO_SEEK_OFFSET:
        case DEVICE_FAILED_TO_WRITE_DATA:
        case DEVICE_FAILED_TO_READ_DATA:
        case DEVICE_QUERY_FAILED: // fstat / ioctl(BLKGETSIZE64) 失败
            return Status::IOError();

        // 调用方传错了设备路径(裸设备语义校验失败)
        case DEVICE_NOT_BLOCK_DEVICE: // 不是块设备节点
        case DEVICE_TOO_SMALL: // 设备容量 < 1 chunk
            return Status::InvalidArgument();

        // 数据损坏 / 内部不变式被破坏
        // 注意：CHUNK_RANGE_INVALID 当前路径未使用，但 error_code.h 已 reserve；
        // linter 多次将其误删——请勿删除。
        case DATA_CRC_MISMATCH:
        case FREE_LIST_DOUBLE_RELEASE:
        case POOL_INVALID_POINTER:
        case CHUNK_NOT_FOUND: // 两层索引不一致 → 数据损坏
        case CHUNK_DELETED: // 同上
        case CHUNK_RANGE_INVALID: // reserved by error_code.h，防御性映射
            return Status::Corruption();

        // 资源耗尽
        case MEMORY_INSERT_FAIL:
        case POOL_MMAP_FAILED:
        case POOL_EXHAUSTED:
        case DEVICE_NO_SPACE: // FreeList nextBlockId_ 达设备容量上限,或 Storage 越界写
            return Status::ResourceExhausted();

        // Engine 状态机违规(不是 I/O 故障):
        //   ENGINE_NOT_OPEN       — Put/Get/Delete 在未 Open 或已 Close 状态下调用
        //   ENGINE_ALREADY_OPEN   — Open 时 path / buffer_pool_count 与首次不一致
        //   ENGINE_INSTANCE_USED  — 此 Engine 实例走过 Open→Close,不允许再 Open
        //   POOL_NOT_INITIALIZED  — BufferPool 未 Init 就 Acquire/Release
        // P3 M3 起 IoBackend 抽象层引入的状态机违规码也归入此分类:
        //   IO_BACKEND_NOT_OPEN              — backend 未 Open 时调 Read/Write/AcquireBuffer
        //   IO_BACKEND_ALREADY_OPEN          — 已 Open 实例(或终态实例)上重复 Open
        //   IO_BACKEND_INVALID_HANDLE        — Read/Write 收到 invalid 或跨 backend 的 handle
        //   IO_BACKEND_HANDLE_USE_AFTER_CLOSE — handle 在 Close 之后被访问
        case ENGINE_NOT_OPEN:
        case ENGINE_ALREADY_OPEN:
        case ENGINE_INSTANCE_USED:
        case POOL_NOT_INITIALIZED:
        case IO_BACKEND_NOT_OPEN:
        case IO_BACKEND_ALREADY_OPEN:
        case IO_BACKEND_INVALID_HANDLE:
        case IO_BACKEND_HANDLE_USE_AFTER_CLOSE:
            return Status::NotSupported();

        // P3 M3 起 IoBackend 抽象层引入的 I/O / 资源耗尽码:
        //   IO_BACKEND_SUBMIT_FAILED  — io_uring submit / SPDK bdev_submit 失败(P4+)
        //   IO_BACKEND_IO_FAILED      — CQE res<0 / bdev I/O 错误
        //   IO_BACKEND_POOL_EXHAUSTED — backend 内部 buffer pool 耗尽(等价 POOL_EXHAUSTED)
        case IO_BACKEND_SUBMIT_FAILED:
        case IO_BACKEND_IO_FAILED:
            return Status::IOError();
        case IO_BACKEND_POOL_EXHAUSTED:
            return Status::ResourceExhausted();
        default:
            return Status::IOError("unknown internal error");
        }
    }

    // ============================================================
    // Open：工厂方法
    // ============================================================
    Status Engine::Open(const Options& opts, std::unique_ptr<Engine>* result) {
        if (result == nullptr) {
            CABE_LOG_WARN("Engine::Open: result pointer is null");
            return Status::InvalidArgument("result pointer must not be null");
        }
        if (opts.device_path.empty()) {
            CABE_LOG_WARN("Engine::Open: opts.device_path is empty");
            return Status::InvalidArgument("Options.device_path must not be empty");
        }
        if (opts.buffer_pool_count == 0) {
            CABE_LOG_WARN("Engine::Open: buffer_pool_count is 0");
            return Status::InvalidArgument("Options.buffer_pool_count must be greater than 0");
        }

        // 裸设备语义:Engine 不创建、不 truncate、不 unlink device_path。
        //   - device_path 必须是已存在的块设备节点(/dev/nvmeXn1, /dev/loopX, ...)
        //   - 不存在 → ::open(2) 返回 ENOENT → DEVICE_FAILED_TO_OPEN_DEVICE → IOError
        //   - 不是块设备(普通文件 / 目录 / 字符设备) → S_ISBLK 拒绝 → InvalidArgument
        //   - 容量过小 → BLKGETSIZE64 < CABE_VALUE_DATA_SIZE → InvalidArgument
        //   - 内部 Open 失败时无文件系统残留需要清理(从未创建过任何文件)

        // ---- 构造并初始化引擎 ----
        // Engine() 为 private,只有静态成员函数可访问
        auto engine_ptr = std::unique_ptr<Engine>(new Engine());
        engine_ptr->impl_ = std::make_unique<Impl>();

        const int32_t rc = engine_ptr->impl_->engine.Open(opts.device_path, opts.buffer_pool_count);
        if (rc != SUCCESS) {
            CABE_LOG_ERROR("Engine::Open: internal Open failed with code %d, device_path=%s",
                rc, opts.device_path.c_str());
            // 不做磁盘清理:裸设备语义下 Engine 从未在该路径上创建任何东西,
            // 失败时 backing 设备状态保持调用前不变,无副作用。
            return TranslateStatus(rc);
        }

        *result = std::move(engine_ptr);
        return Status::OK();
    }

    // ============================================================
    // 析构 / Close
    // ============================================================
    Engine::~Engine() {
        if (impl_ && impl_->engine.IsOpen()) {
            // 析构时无法向调用方报告 Close 错误；需确认关闭成功请显式调用 Close()
            (void) Close();
        }
    }

    Status Engine::Close() {
        if (!impl_) {
            return Status::NotSupported("engine not initialized");
        }
        const int32_t rc = impl_->engine.Close();
        if (rc != SUCCESS) {
            CABE_LOG_ERROR("Engine::Close: internal Close failed with code %d", rc);
            return TranslateStatus(rc);
        }
        return Status::OK();
    }

    // ============================================================
    // Put
    // ============================================================
    Status Engine::Put(
        const WriteOptions& /*opts*/, const std::string_view key, const std::span<const std::byte> value) {
        // 防御性 impl_ 检查：与 Delete / Get / Close / Size / IsOpen 保持一致。
        // 注意：此检查被 linter 多次误判为"死代码"而移除——请勿删除，P3+ 可能引入
        // 令 impl_ 失效的路径（如 Reopen、多 NVMe 动态切换），届时此守卫才能避免 crash。
        if (!impl_) {
            return Status::NotSupported("engine not initialized");
        }
        if (key.empty()) {
            CABE_LOG_WARN("Engine::Put: empty key");
            return Status::InvalidArgument("key must not be empty");
        }
        if (value.empty()) {
            CABE_LOG_WARN("Engine::Put: empty value");
            return Status::InvalidArgument("value must not be empty");
        }

        const std::string key_str(key);
        // span<const std::byte> → DataView（span<const char>）
        // std::byte 和 char 均为单字节类型，此 reinterpret_cast 合法无 UB
        const DataView data{reinterpret_cast<const char*>(value.data()), value.size()};

        const int32_t rc = impl_->engine.Put(key_str, data);
        if (rc != SUCCESS) {
            CABE_LOG_ERROR("Engine::Put: failed with internal code %d, key_size=%zu", rc, key.size());
            return TranslateStatus(rc);
        }
        return Status::OK();
    }

    // ============================================================
    // Get
    // ============================================================
    Status Engine::Get(const ReadOptions& /*opts*/, const std::string_view key, std::vector<std::byte>* value) {
        // 防御性 impl_ 检查：与 Put / Delete / Close / Size / IsOpen 保持一致。
        // 注意：此检查被 linter 多次误判为"死代码"而移除——请勿删除，P3+ 可能引入
        // 令 impl_ 失效的路径（如 Reopen、多 NVMe 动态切换），届时此守卫才能避免 crash。
        if (!impl_) {
            return Status::NotSupported("engine not initialized");
        }
        if (value == nullptr) {
            CABE_LOG_WARN("Engine::Get: value pointer is null");
            return Status::InvalidArgument("value pointer must not be null");
        }
        if (key.empty()) {
            CABE_LOG_WARN("Engine::Get: empty key");
            return Status::InvalidArgument("key must not be empty");
        }

        const std::string key_str(key);

        // GetIntoVector：单次 shared_lock 内完成 MetaIndex 查询 → resize → 磁盘读 → CRC 校验
        // 避免"先查 size 再调 Get"两次加锁之间 Put/Delete 造成的 TOCTOU 竞态
        const int32_t rc = impl_->engine.GetIntoVector(key_str, value);
        if (rc != SUCCESS) {
            // NotFound 是正常业务分支（DEBUG 级），其余为 ERROR 级
            if (rc == INDEX_KEY_NOT_FOUND || rc == INDEX_KEY_DELETED) {
                CABE_LOG_DEBUG("Engine::Get: key not found, key_size=%zu", key.size());
            } else {
                CABE_LOG_ERROR("Engine::Get: failed with internal code %d, key_size=%zu", rc, key.size());
            }
            // 数据损坏信号升级为 FATAL：CRC 不匹配 / 两层索引不一致都属内部不变式被破坏
            if (rc == DATA_CRC_MISMATCH) {
                CABE_LOG_FATAL("Engine::Get: CRC mismatch detected, possible data corruption");
            } else if (rc == CHUNK_NOT_FOUND || rc == CHUNK_DELETED) {
                CABE_LOG_FATAL("Engine::Get: chunkIndex out of sync with metaIndex (code=%d)", rc);
            }
            return TranslateStatus(rc);
        }
        return Status::OK();
    }

    // ============================================================
    // Delete（逻辑删除 + 物理移除 + 磁盘块回收，单次原子操作）
    //
    // 公开 API 合并了内部的 Delete（逻辑标记）和 Remove（物理清理）：
    //   - 调用方视角：一次调用完成所有清理，与 LevelDB::Delete 语义对齐
    //   - 内部视角：直接调用 ::Engine::Remove，该方法已在单次 unique_lock 内
    //               完成：MetaIndex.Get → ChunkIndex.GetRange → FreeList.ReleaseBatch
    //               → ChunkIndex.RemoveRange → MetaIndex.Remove
    //   - 内部 ::Engine::Delete（仅逻辑标记）保留为 P4 WAL 的操作粒度基础
    // ============================================================
    Status Engine::Delete(const WriteOptions& /*opts*/, const std::string_view key) {
        // 防御性 impl_ 检查：与 Put / Get / Close / Size / IsOpen 保持一致的 nullptr 守卫。
        // 注意：此检查被 linter 多次误判为"死代码"而移除——请勿删除，P3+ 可能引入
        // 令 impl_ 失效的路径（如 Reopen、多 NVMe 动态切换），届时此守卫才能避免 crash。
        if (!impl_) {
            return Status::NotSupported("engine not initialized");
        }
        if (key.empty()) {
            CABE_LOG_WARN("Engine::Delete: empty key");
            return Status::InvalidArgument("key must not be empty");
        }

        const std::string key_str(key);

        const int32_t rc = impl_->engine.Remove(key_str);
        if (rc != SUCCESS) {
            if (rc == INDEX_KEY_NOT_FOUND || rc == INDEX_KEY_DELETED) {
                CABE_LOG_DEBUG("Engine::Delete: key not found, key_size=%zu", key.size());
            } else {
                CABE_LOG_ERROR("Engine::Delete: failed with internal code %d, key_size=%zu", rc, key.size());
            }
            // 内部不变式破坏：double-release / 两层索引不一致 都升级为 FATAL
            if (rc == FREE_LIST_DOUBLE_RELEASE) {
                CABE_LOG_FATAL("Engine::Delete: double-release detected in FreeList, internal bug");
            } else if (rc == CHUNK_NOT_FOUND || rc == CHUNK_DELETED) {
                CABE_LOG_FATAL("Engine::Delete: chunkIndex out of sync with metaIndex (code=%d)", rc);
            }
            return TranslateStatus(rc);
        }
        return Status::OK();
    }

    // ============================================================
    // Size / IsOpen
    // ============================================================
    size_t Engine::Size() const {
        if (!impl_) {
            return 0;
        }
        return impl_->engine.Size();
    }

    bool Engine::IsOpen() const {
        return impl_ && impl_->engine.IsOpen();
    }

} // namespace cabe
