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

        bool is_open() const noexcept;

    private:
        std::size_t RouteKey(std::string_view key) const noexcept;
        void TrimDeviceBlock(DeviceContext& dc, BlockId id);

        bool opened_ = false;
        Options options_;                          // P5M3：常驻；Wal/IoBackend 现读 wal_level
        std::vector<DeviceContext> devices_;
    };

} // namespace cabe

#endif // CABE_ENGINE_H
