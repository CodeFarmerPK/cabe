#include <iostream>

// 项目的可执行入口当前是一个 smoke-test 占位,仅验证 cabe_lib 能正常链接。
// P2 C++ API 已经通过 cabe::Engine 对外提供,真实调用方 link cabe_lib 即可。
// P3 io_uring / P4 WAL 落地后,此处可替换为命令行工具(导入/导出/健康检查/
// 崩溃后一致性校验)。
int main() {
    std::cout << "cabe started" << std::endl;
    return 0;
}