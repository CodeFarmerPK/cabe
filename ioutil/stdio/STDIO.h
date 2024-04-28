/*
 * Project: Cabe
 * Created Time: 3/15/24 4:08 PM
 * Created by: CodeFarmerPK
 */

#ifndef CABE_STDIO_H
#define CABE_STDIO_H

#include <cstdint>
#include <iostream>

#include "../../common/error_code.h"
#include "../IOManager.h"

class STDIO : public IOManager {

public:
    STDIO() = default;

    virtual ~STDIO() = default;

    int32_t Open(const std::string &directory, uint64_t fileID, const std::string &fileSuffix, FILE *&file) override;

    int32_t Write(const std::vector<char> &dataVector, int64_t &dataSize, FILE *&file) override;

    int32_t Read(std::vector<char> &valueVector, int64_t offset, size_t &readSize, FILE *&File) override;

    int32_t Sync(FILE *&file) override;

    int32_t Close(FILE *&file) override;

};

#endif //CABE_STDIO_H
