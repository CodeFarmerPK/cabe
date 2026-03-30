/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:31
 * Created by: CodeFarmerPK
 */

#include "engine.h"
#include "util/util.h"
#include "util/crc32.h"

#include <chrono>
#include <cstdlib>
#include <cstring>

// 内存对齐大小（O_DIRECT 要求）
constexpr size_t ALIGNMENT = 512;

Engine::~Engine() {
    if (isOpen_) {
        Close();
    }
}

int32_t Engine::Open(const std::string& devicePath) {
    if (isOpen_) {
        return SUCCESS;
    }

    const int32_t ret = storage_.Open(devicePath);
    if (ret != SUCCESS) {
        return ret;
    }

    isOpen_ = true;
    return SUCCESS;
}

int32_t Engine::Close() {
    if (!isOpen_) {
        return SUCCESS;
    }

    int32_t ret = storage_.Close();
    if (ret != SUCCESS) {
        return ret;
    }

    isOpen_ = false;
    return SUCCESS;
}

int32_t Engine::Put(DataView data, Key* key) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.empty() || key == nullptr) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    // 分配 Key
    const Key newKey = nextKey_++;

    // 分配 BlockId
    BlockId blockId;
    int32_t ret = freeList_.Allocate(&blockId);
    if (ret != SUCCESS) {
        return ret;
    }

    // 分配对齐内存并拷贝数据
    char* alignedBuffer = AllocateAlignedBuffer();
    if (alignedBuffer == nullptr) {
        freeList_.Release(blockId);
        return MEMORY_INSERT_FAIL;
    }
    std::memcpy(alignedBuffer, data.data(), CABE_VALUE_DATA_SIZE);

    // 写入磁盘
    ret =  storage_.WriteBlock(blockId, {alignedBuffer, CABE_VALUE_DATA_SIZE});
    FreeAlignedBuffer(alignedBuffer);

    if (ret != SUCCESS) {
        freeList_.Release(blockId);
        return ret;
    }

    // 计算 CRC
    const uint32_t crc = cabe::util::CRC32(DataView(data));

    // 更新索引
    const IndexEntry entry = {.blockId = blockId, .timestamp = cabe::util::GetTimeStamp(), .crc = crc, .state = DataState::Active};

    ret = index_.Put(newKey, entry);
    if (ret != SUCCESS) {
        freeList_.Release(blockId);
        return ret;
    }

    *key = newKey;
    return SUCCESS;
}

int32_t Engine::Get(const Key key, DataBuffer data) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.empty()) {
        return MEMORY_NULL_POINTER_EXCEPTION;
    }

    // 查询索引
    IndexEntry entry{};
    int32_t ret = index_.Get(key, &entry);
    if (ret != SUCCESS) {
        return ret;
    }

    // 分配对齐内存
    char* alignedBuffer = AllocateAlignedBuffer();
    if (alignedBuffer == nullptr) {
        return MEMORY_INSERT_FAIL;
    }

    // 读取磁盘
    ret = storage_.ReadBlock(entry.blockId, {alignedBuffer, CABE_VALUE_DATA_SIZE});
    if (ret != SUCCESS) {
        FreeAlignedBuffer(alignedBuffer);
        return ret;
    }

    // 校验 CRC
    if (const uint32_t crc = cabe::util::CRC32({alignedBuffer, CABE_VALUE_DATA_SIZE});crc != entry.crc) {
        FreeAlignedBuffer(alignedBuffer);
        return DEVICE_FAILED_TO_READ_DATA;
    }

    // 拷贝数据
    std::memcpy(data.data(), alignedBuffer, CABE_VALUE_DATA_SIZE);
    FreeAlignedBuffer(alignedBuffer);

    return SUCCESS;
}

int32_t Engine::Delete(const Key key) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    return index_.Delete(key);
}

int32_t Engine::Remove(const Key key) {
    if (!isOpen_) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    // 获取索引信息
    IndexEntry entry{};
    if (const int32_t ret = index_.Get(key, &entry);ret != SUCCESS && ret != INDEX_KEY_DELETED) {
        return ret;
    }

    // 如果 Get 返回 INDEX_KEY_DELETED，需要直接查 map 获取 blockId
    // 这里需要特殊处理，先简化：假设 Delete 后仍可获取 entry
    // 回收 BlockId
    freeList_.Release(entry.blockId);

    // 从索引中移除
    return index_.Remove(key);
}

size_t Engine::Size() const {
    return index_.Size();
}

bool Engine::IsOpen() const {
    return isOpen_;
}

char* Engine::AllocateAlignedBuffer() {
    void* buffer = std::aligned_alloc(ALIGNMENT, CABE_VALUE_DATA_SIZE);
    if (buffer != nullptr) {
        std::memset(buffer, 0, CABE_VALUE_DATA_SIZE);
    }
    return static_cast<char*>(buffer);
}

void Engine::FreeAlignedBuffer(char* buffer) {
    if (buffer != nullptr) {
        std::free(buffer);
    }
}
