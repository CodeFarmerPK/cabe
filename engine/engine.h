#ifndef CABE_ENGINE_H
#define CABE_ENGINE_H

#include "engine/device_context.h"
#include "engine/options.h"
#include "engine/status.h"
#include "common/structs.h"

#include <string_view>
#include <vector>

namespace cabe {

    class Engine {
    public:
        Engine() noexcept = default;
        ~Engine();

        Engine(const Engine&) = delete;
        Engine& operator=(const Engine&) = delete;
        Engine(Engine&&) = delete;
        Engine& operator=(Engine&&) = delete;

        Status Open(const Options& opts);
        Status Close();

        Status Put(std::string_view key, DataView value);
        Status Get(std::string_view key, DataBuffer value);
        Status Delete(std::string_view key);

        // P5M3：运行时改 WAL 级别（收紧档时先刷攒批缓冲）。
        Status SetWalLevel(WalLevel level);

        // P5M4：手动触发一份快照（同步执行、返回成败；逐设备各做一份）。
        Status Snapshot();

        bool is_open() const noexcept;

    private:
        std::size_t RouteKey(std::string_view key) const noexcept;
        void TrimDeviceBlock(DeviceContext& dc, BlockId id);

        // P5M4：快照触发链。
        //   DoSnapshot         —— 真活：刷 WAL → 取 covered_seq → snapshot.Write 驱动 ForEach。
        //   RequestSnapshot    —— 自动触发汇总入口（发后不管：出错记日志，不返回、不连累 Put）。
        //                         M4 同步执行；P7 改为唤醒后台快照线程。
        //   MaybeRequestSnapshot —— Put/Delete 成功收尾处查大小阈值，到了就 RequestSnapshot。
        int32_t DoSnapshot(DeviceContext& dc);
        void RequestSnapshot(DeviceContext& dc);
        void MaybeRequestSnapshot(DeviceContext& dc);

        // P5M5：WAL 写入的撞墙救援包装（Put/Delete 共用）——WriteWal 返回 kWalFull 时强制做
        // 一次快照腾空间（直调 DoSnapshot、绕过增长闸门），成功则重试一次；救不了对外 kWalFull。
        int32_t WriteWalRescuing(DeviceContext& dc, const WalEntry& e);

        // ---- P5M6：恢复编排（设计稿 P5M6_recovery_design.md §4，流水线 ③~⑨）----
        // RecoverDevice        —— recover 模式每设备组的恢复编排：快照裁决/加载 → WAL 扫描重放
        //                         → 分配器重建 →（可选）value CRC 全检；任一环错原始码上抛。
        // ApplyRecoveredEntry  —— ④ 快照记录接收器：共享校验 + Insert。
        // ApplyWalEntry        —— ⑥ 活帧接收器：语义解释（Put→Insert / Delete→Delete /
        //                         未知类型与"删不存在"= 证据矛盾拒开）。
        // ValidateRecoveredMeta—— 两条恢复来路共用的统一校验表（4.3：key_len ∈ [1,kWalKeyMax] /
        //                         块号在数据区内 / device 位为 0），违例记 ERROR 三件套返 false。
        // RebuildAllocator     —— ⑧ 终态索引收集活块 → RebuildFromActive（空闲 = 终态补集）。
        // VerifyValuesCrc      —— ⑨ 可选诊断：终态全量读块比 CRC；不符记日志保留条目返成功，
        //                         读 I/O 错返原始码拒开（诊断器不是裁判，P5M6-D16）。
        int32_t RecoverDevice(const DeviceConfig& cfg, DeviceContext& dc);
        int32_t ApplyRecoveredEntry(DeviceContext& dc, std::string_view key, const ValueMeta& meta);
        int32_t ApplyWalEntry(DeviceContext& dc, const WalEntry& e, std::uint64_t seq);
        bool ValidateRecoveredMeta(const DeviceContext& dc, std::string_view key, BlockId block,
                                   bool has_block, const char* origin, std::uint64_t seq) const;
        int32_t RebuildAllocator(DeviceContext& dc);
        int32_t VerifyValuesCrc(DeviceContext& dc);

        // P5M4：Open 失败路径统一清理——关当前还未入列的局部 dc + 已入列设备，清空列表。
        void AbortOpen(DeviceContext& dc);

        bool opened_ = false;
        Options options_;                          // P5M3：常驻；Wal/IoBackend 现读 wal_level
        std::vector<DeviceContext> devices_;
    };

} // namespace cabe

#endif // CABE_ENGINE_H
