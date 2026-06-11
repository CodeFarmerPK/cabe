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
        //   RequestSnapshot    —— 自动触发汇总入口（fire-and-forget：出错记日志，不返回、不连累 Put）。
        //                         M4 同步执行；P7 改为唤醒后台快照线程。
        //   MaybeRequestSnapshot —— Put/Delete 成功收尾处查大小阈值，到了就 RequestSnapshot。
        int32_t DoSnapshot(DeviceContext& dc);
        void RequestSnapshot(DeviceContext& dc);
        void MaybeRequestSnapshot(DeviceContext& dc);

        // P5M5：WAL 写入的撞墙救援包装（Put/Delete 共用）——WriteWal 返回 kWalFull 时强制做
        // 一次快照腾空间（直调 DoSnapshot、绕过增长闸门），成功则重试一次；救不了对外 kWalFull。
        int32_t WriteWalRescuing(DeviceContext& dc, const WalEntry& e);

        // P5M4：Open 失败路径统一清理——关当前还未入列的局部 dc + 已入列设备，清空列表。
        void AbortOpen(DeviceContext& dc);

        bool opened_ = false;
        Options options_;                          // P5M3：常驻；Wal/IoBackend 现读 wal_level
        std::vector<DeviceContext> devices_;
    };

} // namespace cabe

#endif // CABE_ENGINE_H
