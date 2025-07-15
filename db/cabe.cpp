//
// Created by root on 2025-07-15.
//

#include "cabe.h"

#include "util/util.h"
int32_t Cabe::Put(std::string_view keyString, const std::span<char>& valueSpan) {
    if (keyString.empty()) {
        return CABE_EMPTY_KEY;
    }
    if (valueSpan.empty()) {
        return CABE_EMPTY_VALUE;
    }

    int32_t status = stdIO->Write(valueSpan, dataOffset, fd);
    if (status != SUCCESS) {
        return status;
    }

    MemoryIndex memoryIndex{dataOffset, GetTimeStamp()};
    status = Index::Put(keyString, memoryIndex);
    if (status != SUCCESS) {
        return status;
    }

    return SUCCESS;
}

int32_t Cabe::Get(std::string_view keyString, std::span<char>& valueSpan) {
    return SUCCESS;
}

int32_t Cabe::Delete(std::string_view keyString) {
    return SUCCESS;
}
