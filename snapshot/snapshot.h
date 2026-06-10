#ifndef CABE_SNAPSHOT_H
#define CABE_SNAPSHOT_H

// P5M4：快照模块（设计依据：doc/P5/P5M4_snapshot_design.md §9）。
// 与 wal/、io/ 平级——三设备三管理模块（数据 IoBackend / WAL Wal / 快照 Snapshot）。
// 结构无关：通过回调（SnapshotScanFn）驱动遍历，不认识任何 MetaIndex 后端类型。
// M4 只实现写流程（Open / Write / Close）；加载（Load）+ 完整恢复在 M6。

#include "snapshot/snapshot_format.h"
#include "engine/options.h"      // Options
#include "engine/super_block.h"  // SuperBlock（取 block_count + engine_uuid）
#include "index/meta_index.h"    // MetaIndexVisitor
#include "util/raw_device.h"

#include <cstdint>
#include <functional>
#include <string>

namespace cabe {

    // "拿快照给的访问器去遍历内存索引"——由 Engine 包成 lambda 注入（内部调 index.ForEach）。
    // 返回 int32_t：把 ForEach 的可中止错误一路传出（与 MetaIndexVisitor 一致）。
    using SnapshotScanFn = std::function<int32_t(const MetaIndexVisitor&)>;

    class Snapshot {
    public:
        Snapshot() noexcept = default;
        ~Snapshot() = default;

        Snapshot(const Snapshot&) = delete;
        Snapshot& operator=(const Snapshot&) = delete;
        Snapshot(Snapshot&&) noexcept = default;            // RawDevice 自带 noexcept 移动；其余成员可平凡移动
        Snapshot& operator=(Snapshot&&) noexcept = default;

        // 打开快照设备 + 算 A/B 布局常驻 + 部署期容量校验 + （create）清空两槽、next_gen=1。
        // data_sb：所属数据设备的超级块（取 block_count 做容量校验、engine_uuid 盖槽头）。
        // 注：M4 仅 create 模式调用；recover 模式的"读两槽头重建 active/代际"在 M6。
        int32_t Open(const std::string& path, const Options* opts, const SuperBlock& data_sb);

        // 写一份新快照到非活跃槽：流式遍历编码 → 末尾补零 → 写槽头 → 一次 fdatasync。
        // 成功才更新内存缓存（active_slot/next_gen/last_covered_seq）。
        int32_t Write(std::uint64_t covered_seq, const SnapshotScanFn& scan);

        int32_t Close();

        // 记账一次快照尝试：推进退避基准（成败都算，设计 §10.3）。由 Engine::DoSnapshot 在
        // 入口调用——放在尝试的起点，保证"还没到 Write 就失败"的路径（如刷 WAL 失败）也被记账。
        void NoteTriggerAttempt(std::uint64_t seq) noexcept { last_trigger_seq_ = seq; }

        bool is_open() const noexcept { return dev_.is_open(); }
        std::uint64_t last_covered_seq() const noexcept { return last_covered_seq_; } // 上次已落地快照覆盖点（M5 回收用）
        std::uint64_t last_trigger_seq() const noexcept { return last_trigger_seq_; } // 触发基准（NoteTriggerAttempt 推进，退避用）

    private:
        RawDevice      dev_;                  // 自持快照设备（不走 IoBackend）
        std::uint64_t  slot_a_off_ = 0;       // A 槽起点（= kDataRegionOffset）
        std::uint64_t  slot_b_off_ = 0;       // B 槽起点（= A 起点 + slot_size_）
        std::uint64_t  slot_size_  = 0;       // 单槽大小（对半切、向下取整 4K）
        std::uint64_t  next_gen_   = 1;       // 下一份快照的代际号
        std::uint64_t  last_covered_seq_ = 0; // 上次成功快照的 covered_seq
        std::uint64_t  last_trigger_seq_ = 0; // 上次尝试快照时的 seq（退避基准）
        int            active_slot_ = -1;     // 当前最新可恢复槽：0=A，1=B，-1=无
        std::uint8_t   engine_uuid_[16]{};    // 盖进槽头 + 加载比对
        const Options* opts_ = nullptr;       // 现读 snapshot_buffer_size
    };

} // namespace cabe

#endif // CABE_SNAPSHOT_H
