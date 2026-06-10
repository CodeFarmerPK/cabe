#ifndef CABE_TEST_ENV_H
#define CABE_TEST_ENV_H

// P5M4：测试公共小工具——环境变量读取（设备测试三路径约定见 scripts/mkloop.sh）。
// 此前 GetEnv 在各测试文件逐字拷贝（wal/engine/super_block/snapshot 四处），收敛到此一份。
// （bench/engine_bench.cpp 仍保留自己的拷贝——bench 是独立目标，不依赖 test/ 头文件。）

#include <cstdlib>
#include <string>

namespace cabe::test {

    inline std::string GetEnv(const char* name) {
        const char* v = std::getenv(name);
        return v ? std::string(v) : "";
    }

} // namespace cabe::test

#endif // CABE_TEST_ENV_H
