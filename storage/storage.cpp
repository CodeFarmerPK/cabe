/*
 * Project: Cabe
 * Created Time: 2026-03-30 15:21
 * Created by: CodeFarmerPK
 */

#include "storage.h"
#include <fcntl.h>
#include <unistd.h>

Storage::~Storage() {
    if (fd_ >= 0) {
        Close();
    }
}

int32_t Storage::Open(const std::string& devicePath) {
    if (devicePath.empty()) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    devicePath_ = devicePath;

    fd_ = ::open(devicePath.c_str(), O_RDWR | O_DIRECT | O_SYNC);
    if (fd_ < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    return SUCCESS;
}

int32_t Storage::Close() {
    if (fd_ < 0) {
        return SUCCESS;
    }

    if (::close(fd_) < 0) {
        return DEVICE_FAILED_TO_CLOSE_DEVICE;
    }

    fd_ = -1;
    return SUCCESS;
}

int32_t Storage::WriteBlock(const BlockId blockId,const DataView data) const {
    if (fd_ < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;

    // pwrite: 原子性地定位并写入，线程安全
    if (const ssize_t written = ::pwrite(fd_, data.data(), CABE_VALUE_DATA_SIZE, offset); written < 0 || static_cast<size_t>(written) != CABE_VALUE_DATA_SIZE) {
        return DEVICE_FAILED_TO_WRITE_DATA;
    }

    return SUCCESS;
}

int32_t Storage::ReadBlock(const BlockId blockId, DataBuffer data) const {
    if (fd_ < 0) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }

    if (data.size() != CABE_VALUE_DATA_SIZE) {
        return CABE_INVALID_DATA_SIZE;
    }

    const off_t offset = static_cast<off_t>(blockId) * CABE_VALUE_DATA_SIZE;

    // pread: 原子性地定位并读取，线程安全
    if (const ssize_t bytesRead = ::pread(fd_, data.data(), CABE_VALUE_DATA_SIZE, offset);bytesRead < 0 || static_cast<size_t>(bytesRead) != CABE_VALUE_DATA_SIZE) {
        return DEVICE_FAILED_TO_READ_DATA;
    }

    return SUCCESS;
}

bool Storage::IsOpen() const {
    return fd_ >= 0;
}