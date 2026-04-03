/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:37
 * Created by: CodeFarmerPK
 */
#ifndef ERROR_CODE_H
#define ERROR_CODE_H

#include <cstdint>

#define SUCCESS 0

// memory codes
#define MEMORY_NULL_POINTER_EXCEPTION (-100000)
#define MEMORY_EMPTY_KEY              (-100001)
#define MEMORY_EMPTY_VALUE            (-100002)
#define MEMORY_INSERT_FAIL            (-100003)

// index codes
#define INDEX_KEY_NOT_FOUND (-200000)
#define INDEX_KEY_DELETED   (-200001)

#define CHUNK_NOT_FOUND     (-300000)
#define CHUNK_DELETED       (-300001)
#define CHUNK_RANGE_INVALID (-300002)

// device codes
#define DEVICE_FAILED_TO_OPEN_DEVICE  (-400000)
#define DEVICE_FAILED_TO_CLOSE_DEVICE (-400001)
#define DEVICE_FAILED_TO_SEEK_OFFSET  (-400002)
#define DEVICE_FAILED_TO_WRITE_DATA   (-400003)
#define DEVICE_FAILED_TO_READ_DATA    (-400004)

// cabe codes
#define CABE_EMPTY_KEY         (-500001)
#define CABE_EMPTY_VALUE       (-500002)
#define CABE_INVALID_DATA_SIZE (-500003)

// crc codes
#define DATA_CRC_MISMATCH (-600001)
#endif // ERROR_CODE_H
