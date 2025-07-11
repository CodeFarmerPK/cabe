/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:37
 * Created by: CodeFarmerPK
 */
#ifndef ERROR_CODE_H
#define ERROR_CODE_H

#include <cstdint>

#define SUCCESS 0

#define DATA_SIZE 32

// memory codes
#define MEMORY_NULL_POINTER_EXCEPTION (-100000)
#define MEMORY_EMPTY_KEY (-100001)
#define MEMORY_EMPTY_VALUE (-100002)
#define MEMORY_INSERT_FAIL (-100003)

// device codes
#define DEVICE_FAILED_TO_OPEN_DEVICE (-200000)
#define DEVICE_FAILED_TO_SEEK_OFFSET (-200001)
#define DEVICE_FAILED_TO_WRITE_DATA (-200002)
#define DEVICE_FAILED_TO_READ_DATA (-200002)

#endif // ERROR_CODE_H
