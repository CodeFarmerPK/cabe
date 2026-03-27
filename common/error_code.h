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
#define MEMORY_EMPTY_KEY (-100001)
#define MEMORY_EMPTY_VALUE (-100002)
#define MEMORY_INSERT_FAIL (-100003)

// index codes
#define INDEX_KEY_NOT_FOUND (-200000)
#define INDEX_KEY_DELETED (-200001)

// device codes
#define DEVICE_FAILED_TO_OPEN_DEVICE (-300000)
#define DEVICE_FAILED_TO_SEEK_OFFSET (-300001)
#define DEVICE_FAILED_TO_WRITE_DATA (-300002)
#define DEVICE_FAILED_TO_READ_DATA (-300002)

// cabe codes
#define CABE_EMPTY_KEY (-400001)
#define CABE_EMPTY_VALUE (-400002)

#endif // ERROR_CODE_H
