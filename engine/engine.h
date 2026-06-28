#ifndef CABE_ENGINE_H
#define CABE_ENGINE_H

#include "engine/reactor.h"          // P7M1：Reactor（含 device_context.h）
#include "engine/device_context.h"
#include "engine/options.h"
#include "engine/status.h"
#include "common/structs.h"

#include <atomic>
#include <cstdint>
#include <memory>
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

        // P5M3：运行时改 WAL 级别。
        Status SetWalLevel(WalLevel level);
        // P5M4：手动触发一份快照。
        Status Snapshot();
        // P7M1：Put / Delete / SetWalLevel / Snapshot 在 M1 暂返 kEngineNotImplemented——
        //   写路径与运营口随 M2 入 reactor。M1 能用的公开 API = Open / Get / Close / is_open。

        bool is_open() const noexcept;

    private:
        std::size_t RouteKey(std::string_view key) const noexcept;
        // P7M2：写路径辅助（DoSnapshot/WriteWalRescuing/RequestSnapshot/MaybeRequestSnapshot/
        //   TrimDeviceBlock）已迁入 Reactor（以 dc_/options_ 为隐式对象，share-nothing）。

        // ---- P5M6：恢复编排（Open 阶段一在裸 dc 上跑，reactor 线程尚未起；签名不变）----
        int32_t RecoverDevice(const DeviceConfig& cfg, DeviceContext& dc);
        int32_t ApplyRecoveredEntry(DeviceContext& dc, std::string_view key, const ValueMeta& meta);
        int32_t ApplyWalEntry(DeviceContext& dc, const WalEntry& e, std::uint64_t seq);
        bool ValidateRecoveredMeta(const DeviceContext& dc, std::string_view key, BlockId block,
                                   bool has_block, const char* origin, std::uint64_t seq) const;
        int32_t RebuildAllocator(DeviceContext& dc);
        int32_t VerifyValuesCrc(DeviceContext& dc);

        // P7M1：关一个裸 dc 的三设备句柄（Open 失败路径用；两阶段的 recovered 临时表由 Open 自己遍历）。
        void AbortOpen(DeviceContext& dc);

        std::atomic<bool> opened_{false};
        Options options_;                                  // 常驻；reactor 内组件持 &options_ 现读（M1 只读）
        std::vector<std::unique_ptr<Reactor>> reactors_;   // P7M1：每 device 一个 reactor（N=1，不可移动 → unique_ptr）
    };

} // namespace cabe

#endif // CABE_ENGINE_H
