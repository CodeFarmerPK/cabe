//
// Created by root on 2025-07-15.
//

#ifndef CABE_CABE_H
#define CABE_CABE_H


#include "localstore/std_io.h"
#include "memory/index.h"
#include <fcntl.h>
#include <span>

class Cabe {
    explicit Cabe(Index* cabeIndex, std_io* stdIO) {
        // todo 单例模式重写构造函数
        fd = open("/dev/nvme2n1", O_RDWR);
        dataOffset = 0;

        Cabe::stdIO = stdIO;
    }

    virtual ~Cabe() = default;

    virtual int32_t Put(std::string_view keyString, const std::span<char>& valueSpan);

    virtual int32_t Get(std::string_view keyString, std::span<char>& valueSpan);

    virtual int32_t Delete(std::string_view keyString);

private:
    static int fd;
    static int64_t dataOffset;

    static std_io* stdIO;
};


#endif // CABE_CABE_H
