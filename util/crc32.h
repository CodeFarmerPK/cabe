/*
 * Project: Cabe
 * Created Time: 2025-05-16 14:51
 * Created by: CodeFarmerPK
 */

#ifndef CRC32_H
#define CRC32_H

#include "common/error_code.h"
#include "common/structs.h"
#include <span>

namespace cabe::util {
    uint32_t CRC32(DataView data);
}


#endif
