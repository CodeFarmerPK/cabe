/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:37
 * Created by: CodeFarmerPK
 */
#ifndef CABE_ERROR_CODE_H
#define CABE_ERROR_CODE_H

#include <cstdint>

#define SUCCESS 0

// memory codes
#define MEMORY_NULL_POINTER_EXCEPTION (-100000)
#define MEMORY_EMPTY_KEY              (-100001)
#define MEMORY_EMPTY_VALUE            (-100002)
#define MEMORY_INSERT_FAIL            (-100003)
#endif // CABE_ERROR_CODE_H