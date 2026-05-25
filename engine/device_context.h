#ifndef CABE_DEVICE_CONTEXT_H
#define CABE_DEVICE_CONTEXT_H

// 内部类型，不在公开 API 承诺内。
// P1M1 只含 fd；后续 milestone 逐步补入 BufferPool / FreeList / MetaIndex。

namespace cabe {

    struct DeviceContext {
        int fd = -1;
    };

} // namespace cabe

#endif // CABE_DEVICE_CONTEXT_H
