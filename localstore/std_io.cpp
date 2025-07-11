//
// Created by root on 2025-07-11.
//

#include "std_io.h"

#include <fcntl.h>
#include <unistd.h>

int32_t std_io::Open(const std::string& devicePath) {
    fd = open(devicePath.c_str(), O_RDWR);
    if (fd == -1) {
        return DEVICE_FAILED_TO_OPEN_DEVICE;
    }
    return SUCCESS;
}
int32_t std_io::Write(const std::span<char>& dataSpan, int64_t offset) {
    if (lseek(fd, offset, SEEK_SET) == -1) {
        return DEVICE_FAILED_TO_SEEK_OFFSET;
    }
    ssize_t bytes_written = write(fd, dataSpan.data(), DATA_SIZE);
    if (bytes_written == -1) {
        return DEVICE_FAILED_TO_WRITE_DATA;
    }
    return SUCCESS;
}
int32_t std_io::Read(const std::span<char>& dataSpan, int64_t offset) {
    if (lseek(fd, offset, SEEK_SET) == -1) {
        return DEVICE_FAILED_TO_SEEK_OFFSET;
    }

    ssize_t bytes_read = read(fd, dataSpan.data(), DATA_SIZE);
    if (bytes_read == -1) {
        return DEVICE_FAILED_TO_READ_DATA;
    }
    return SUCCESS;
}
