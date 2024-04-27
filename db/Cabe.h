/*
 * Project: Cabe
 * Created Time: 3/19/24 11:58 PM
 * Created by: CodeFarmerPK
 */

#ifndef CABE_CABE_H
#define CABE_CABE_H

#include <cstdint>
#include <string>
#include <cstring>
#include <filesystem>
#include <algorithm>
#include <vector>
#include <map>

#include "../common/structs.h"
#include "../memory/Index.h"
#include "../memory/rbtree/HashIndex.h"
#include "../ioutil//stdio/STDIO.h"
#include "../util/UtilMethods.h"


class Cabe {
public:
    Cabe(IOManager *cabeIoManager, Index *cabeIndex, Options cabeOptions) {
        // todo 单例模式重写构造函数
        Cabe::ioManager = cabeIoManager;
        Cabe::index = cabeIndex;
        Cabe::dbOptions = cabeOptions;
        Cabe::metadataSize = sizeof(Metadata);
        Cabe::mergeFlag.mergeStart = false;
        Cabe::mergeFlag.mergeStartTime = 0;
    }

    virtual ~Cabe() = default;

    virtual int32_t Put(const std::vector<char> &keyVector, const std::vector<char> &valueVector);

    virtual int32_t Get(const std::vector<char> &keyVector, std::vector<char> &valueVector);

    virtual int32_t Delete(const std::vector<char> &keyVector);

    virtual int32_t Merge();

    virtual int32_t Persist();

    virtual int32_t AppendData(const std::vector<char> &dataVector);

    virtual int32_t LoadDataFile();

private:
    static FILE *activeFile;
    static FileInfo *activeFileInfo;
    static Index *index;
    static IOManager *ioManager;
    static Options dbOptions;
    static uint32_t fileId;
    static int32_t metadataSize;
    static MergeFlag mergeFlag;
    static std::map<uint32_t, FILE *> inactiveFiles;
    static std::map<uint32_t, FILE *> mergedFiles;
};

#endif
