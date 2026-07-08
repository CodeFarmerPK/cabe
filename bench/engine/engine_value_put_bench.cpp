// P8M5: value-source Put microbenchmarks.
//
// The target keeps the historical bench_engine target unchanged and adds a
// focused Put matrix for the three P8 value sources:
//   1. unaligned external memory  -> copy fallback path
//   2. aligned external memory    -> conditional direct path
//   3. Cabe ValueBuffer           -> main zero-copy path
//
// Allocation and value filling are outside the measured interval. The measured
// body is Engine::Put only. The numbers are archived as path-observation data,
// not as a loop-device performance conclusion.

#include "engine/engine.h"

#include <benchmark/benchmark.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr int kSlots = 16;
constexpr std::size_t kExternalAlignment = cabe::kValueSize;

std::string GetEnv(const char* name) {
    const char* v = std::getenv(name);
    return v ? std::string(v) : "";
}

std::byte FillByte(std::byte base, std::uint64_t seq) {
    return static_cast<std::byte>((static_cast<unsigned>(base) + (seq & 0xFFu)) & 0xFFu);
}

void Fill(cabe::DataBuffer buffer, std::byte fill) {
    std::memset(buffer.data(), static_cast<int>(fill), buffer.size());
}

void Fill(cabe::DataView view, std::byte fill) {
    std::memset(const_cast<std::byte*>(view.data()), static_cast<int>(fill), view.size());
}

bool IsAligned(const void* ptr, std::size_t alignment) {
    if (ptr == nullptr || alignment == 0) return false;
    return reinterpret_cast<std::uintptr_t>(ptr) % alignment == 0;
}

struct FreeDeleter {
    void operator()(std::byte* ptr) const noexcept {
        std::free(ptr);
    }
};

using AlignedBytes = std::unique_ptr<std::byte, FreeDeleter>;

AlignedBytes MakeAlignedValue(std::byte fill) {
    void* raw = nullptr;
    if (posix_memalign(&raw, kExternalAlignment, cabe::kValueSize) != 0) return {};
    AlignedBytes value(static_cast<std::byte*>(raw));
    std::memset(value.get(), static_cast<int>(fill), cabe::kValueSize);
    return value;
}

struct UnalignedSlot {
    std::string key;
    std::vector<std::byte> storage;
    std::size_t offset = 1;

    cabe::DataView view() const {
        return cabe::DataView{storage.data() + offset, cabe::kValueSize};
    }
};

struct AlignedSlot {
    std::string key;
    AlignedBytes storage;

    cabe::DataView view() const {
        return cabe::DataView{storage.get(), cabe::kValueSize};
    }
};

struct ValueBufferSlot {
    std::string key;
    cabe::ValueBuffer buffer;

    cabe::DataView view() const {
        return buffer.view();
    }
};

class EngineValuePutBench : public benchmark::Fixture {
public:
    void SetUp(benchmark::State& state) override {
        data_ = GetEnv("CABE_TEST_DEVICE");
        wal_  = GetEnv("CABE_TEST_WAL_DEVICE");
        snap_ = GetEnv("CABE_TEST_SNAPSHOT_DEVICE");
        if (data_.empty() || wal_.empty() || snap_.empty()) {
            state.SkipWithMessage("需要 CABE_TEST_DEVICE / CABE_TEST_WAL_DEVICE / CABE_TEST_SNAPSHOT_DEVICE");
            return;
        }

        const int64_t lvl = state.range(0);
        if (!cabe::IsValidWalLevel(static_cast<cabe::WalLevel>(lvl))) {
            state.SkipWithMessage("WAL 级别非法（须 1/2/3/4）");
            return;
        }

        const auto s = engine_.Open(MakeOpts(static_cast<int>(lvl)));
        if (!s.ok()) {
            state.SkipWithMessage("Engine::Open 失败");
            return;
        }
        open_ok_ = true;
    }

    void TearDown(benchmark::State&) override {
        if (open_ok_) engine_.Close();
        open_ok_ = false;
    }

protected:
    cabe::Options MakeOpts(int level) const {
        cabe::Options opts;
        opts.devices.push_back({data_, wal_, snap_});
        opts.create = true;
        opts.wal_level = static_cast<cabe::WalLevel>(level);
        opts.snapshot_threshold_bytes = 1ull * 1024 * 1024;
        opts.value_buffer_pool_blocks = kSlots;
        return opts;
    }

    bool PrepareUnalignedSlots(benchmark::State& state, std::vector<UnalignedSlot>& slots) {
        slots.clear();
        slots.reserve(kSlots);
        for (int i = 0; i < kSlots; ++i) {
            UnalignedSlot slot;
            slot.key = "p8-unaligned-" + std::to_string(i);
            slot.storage.resize(cabe::kValueSize + 4096);
            slot.offset = 1;
            if (IsAligned(slot.storage.data() + slot.offset, 4096) ||
                IsAligned(slot.storage.data() + slot.offset, cabe::kValueSize)) {
                slot.offset = 2;
            }
            Fill(slot.view(), std::byte{0xA1});
            const auto s = engine_.Put(slot.key, slot.view());
            if (!s.ok()) {
                state.SkipWithMessage("预写非对齐自备值内存失败");
                return false;
            }
            slots.push_back(std::move(slot));
        }
        return true;
    }

    bool PrepareAlignedSlots(benchmark::State& state, std::vector<AlignedSlot>& slots) {
        slots.clear();
        slots.reserve(kSlots);
        for (int i = 0; i < kSlots; ++i) {
            AlignedSlot slot;
            slot.key = "p8-aligned-" + std::to_string(i);
            slot.storage = MakeAlignedValue(std::byte{0xB2});
            if (!slot.storage || !IsAligned(slot.storage.get(), kExternalAlignment)) {
                state.SkipWithMessage("分配对齐自备值内存失败");
                return false;
            }
            const auto s = engine_.Put(slot.key, slot.view());
            if (!s.ok()) {
                state.SkipWithMessage("预写对齐自备值内存失败");
                return false;
            }
            slots.push_back(std::move(slot));
        }
        return true;
    }

    bool PrepareValueBufferSlots(benchmark::State& state, std::vector<ValueBufferSlot>& slots) {
        slots.clear();
        slots.reserve(kSlots);
        for (int i = 0; i < kSlots; ++i) {
            ValueBufferSlot slot;
            slot.key = "p8-value-buffer-" + std::to_string(i);
            auto r = engine_.AllocateValueBuffer(slot.key);
            if (!r.ok()) {
                state.SkipWithMessage("分配 Cabe ValueBuffer 失败");
                return false;
            }
            slot.buffer = std::move(r.buffer);
            Fill(slot.buffer.data(), std::byte{0xC3});
            const auto s = engine_.Put(slot.key, slot.view());
            if (!s.ok()) {
                state.SkipWithMessage("预写 Cabe ValueBuffer 失败");
                return false;
            }
            slots.push_back(std::move(slot));
        }
        return true;
    }

    cabe::Engine engine_;
    std::string data_;
    std::string wal_;
    std::string snap_;
    bool open_ok_ = false;
};

} // namespace

BENCHMARK_DEFINE_F(EngineValuePutBench, BM_PutUnalignedExternal)(benchmark::State& state) {
    std::vector<UnalignedSlot> slots;
    state.PauseTiming();
    const bool prepared = PrepareUnalignedSlots(state, slots);
    state.ResumeTiming();
    if (!prepared) return;

    std::uint64_t seq = 0;
    for (auto _ : state) {
        auto& slot = slots[seq % slots.size()];
        state.PauseTiming();
        Fill(slot.view(), FillByte(std::byte{0xA1}, seq));
        state.ResumeTiming();

        const auto s = engine_.Put(slot.key, slot.view());
        if (!s.ok()) {
            state.SkipWithMessage("非对齐自备值内存 Put 失败");
            break;
        }
        ++seq;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
BENCHMARK_REGISTER_F(EngineValuePutBench, BM_PutUnalignedExternal)
    ->Unit(benchmark::kMillisecond)
    ->Arg(1)->Arg(2)->Arg(3)->Arg(4)
    ->UseRealTime();

BENCHMARK_DEFINE_F(EngineValuePutBench, BM_PutAlignedExternal)(benchmark::State& state) {
    std::vector<AlignedSlot> slots;
    state.PauseTiming();
    const bool prepared = PrepareAlignedSlots(state, slots);
    state.ResumeTiming();
    if (!prepared) return;

    std::uint64_t seq = 0;
    for (auto _ : state) {
        auto& slot = slots[seq % slots.size()];
        state.PauseTiming();
        Fill(slot.view(), FillByte(std::byte{0xB2}, seq));
        state.ResumeTiming();

        const auto s = engine_.Put(slot.key, slot.view());
        if (!s.ok()) {
            state.SkipWithMessage("对齐自备值内存 Put 失败");
            break;
        }
        ++seq;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
BENCHMARK_REGISTER_F(EngineValuePutBench, BM_PutAlignedExternal)
    ->Unit(benchmark::kMillisecond)
    ->Arg(1)->Arg(2)->Arg(3)->Arg(4)
    ->UseRealTime();

BENCHMARK_DEFINE_F(EngineValuePutBench, BM_PutValueBuffer)(benchmark::State& state) {
    std::vector<ValueBufferSlot> slots;
    state.PauseTiming();
    const bool prepared = PrepareValueBufferSlots(state, slots);
    state.ResumeTiming();
    if (!prepared) return;

    std::uint64_t seq = 0;
    for (auto _ : state) {
        auto& slot = slots[seq % slots.size()];
        state.PauseTiming();
        Fill(slot.buffer.data(), FillByte(std::byte{0xC3}, seq));
        state.ResumeTiming();

        const auto s = engine_.Put(slot.key, slot.view());
        if (!s.ok()) {
            state.SkipWithMessage("Cabe ValueBuffer Put 失败");
            break;
        }
        ++seq;
        benchmark::ClobberMemory();
    }
    state.SetItemsProcessed(state.iterations());
    state.SetBytesProcessed(state.iterations() * cabe::kValueSize);
}
BENCHMARK_REGISTER_F(EngineValuePutBench, BM_PutValueBuffer)
    ->Unit(benchmark::kMillisecond)
    ->Arg(1)->Arg(2)->Arg(3)->Arg(4)
    ->UseRealTime();
