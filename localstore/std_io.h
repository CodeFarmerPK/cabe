//
// Created by root on 2025-07-11.
//

#ifndef CABE_STD_IO_H
#define CABE_STD_IO_H

#include "common/error_code.h"
#include <cstdio>
#include <span>
#include <string>

class std_io {
public:
    std_io() = default;

    virtual ~std_io() = default;

    int32_t Open(const std::string& devicePath);

    int32_t Write(const std::span<char>& dataSpan, int64_t offset);

    int32_t Read(const std::span<char>& dataSpan, int64_t offset);

private:
    int fd = 0;
};


#endif // CABE_STD_IO_H
