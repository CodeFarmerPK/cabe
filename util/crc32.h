/*
 * Project: Cabe
 * Created Time: 2025-05-16 14:51
 * Created by: CodeFarmerPK
 */

#ifndef CABE_CRC32_H
#define CABE_CRC32_H

#include "common/structs.h"

namespace cabe::util {
    uint32_t CRC32(DataView data);
}


#endif // CABE_CRC32_H
