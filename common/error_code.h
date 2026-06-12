/*
 * Project: Cabe
 * Created Time: 2025-05-16 17:37
 * Created by: CodeFarmerPK
 */
#ifndef CABE_ERROR_CODE_H
#define CABE_ERROR_CODE_H

#include <cstdint>

namespace cabe::err {
    inline constexpr int kSuccess = 0;

    // ---- 段基址与容量（每段 1000 号；错误码为负，段内向更负方向编号）----
    inline constexpr int kSegmentSize     = 1000;
    inline constexpr int kMemoryBase      = -100000;
    inline constexpr int kIoBase          = -101000;
    inline constexpr int kIndexBase       = -102000;
    inline constexpr int kWalBase         = -103000;
    inline constexpr int kEngineBase      = -104000;
    inline constexpr int kWalRecoveryBase = -105000;
    inline constexpr int kSnapshotBase    = -106000;

    // ---- 段位不重叠（编译期）：相邻段恰好相距一个段容量，无缝且不交叠 ----
    static_assert(kMemoryBase - kSegmentSize == kIoBase);
    static_assert(kIoBase - kSegmentSize == kIndexBase);
    static_assert(kIndexBase - kSegmentSize == kWalBase);
    static_assert(kWalBase - kSegmentSize == kEngineBase);
    static_assert(kEngineBase - kSegmentSize == kWalRecoveryBase);
    static_assert(kWalRecoveryBase - kSegmentSize == kSnapshotBase);

    // 段内编号：基址段的第 n 个码（n ∈ [0, kSegmentSize)）
    constexpr int InSeg(int base, int n) noexcept { return base - n; }

    // ---- memory 段（保留现有取值）----
    inline constexpr int kMemNullPointer = InSeg(kMemoryBase, 0); // -100000
    inline constexpr int kMemEmptyKey    = InSeg(kMemoryBase, 1); // -100001
    inline constexpr int kMemEmptyValue  = InSeg(kMemoryBase, 2); // -100002
    inline constexpr int kMemInsertFail  = InSeg(kMemoryBase, 3); // -100003

    // 每个码不得越段（编译期）
    static_assert(kMemInsertFail > kMemoryBase - kSegmentSize);

    // ---- engine 段（P1M1 新增）----
    inline constexpr int kEngineAlreadyOpen    = InSeg(kEngineBase, 0);  // -104000
    inline constexpr int kEngineNotOpen        = InSeg(kEngineBase, 1);  // -104001
    inline constexpr int kEngineInvalidOpts    = InSeg(kEngineBase, 2);  // -104002
    inline constexpr int kEngineInvalidValue   = InSeg(kEngineBase, 3);  // -104003
    inline constexpr int kEngineNotImplemented = InSeg(kEngineBase, 4);  // -104004

    inline constexpr int kEngineNoSpace         = InSeg(kEngineBase, 5);  // -104005
    inline constexpr int kEnginePoolExhausted   = InSeg(kEngineBase, 6);  // -104006
    inline constexpr int kEngineDataCorrupted   = InSeg(kEngineBase, 7);  // -104007
    // P5M6（分配器沿 kEngineNoSpace 先例用 engine 段）：RebuildFromActive 的活块清单矛盾——
    // 越界/重复都意味着恢复出的索引不可信（穿透了 CRC 的盘上矛盾或上游 bug），拒开级。
    inline constexpr int kEngineDuplicateBlock  = InSeg(kEngineBase, 8);  // -104008  两个键声称独占同一物理块
    inline constexpr int kEngineBlockOutOfRange = InSeg(kEngineBase, 9);  // -104009  活块块号越出数据区（原静默跳过，P5M6 升级为报错）

    static_assert(kEngineBlockOutOfRange > kEngineBase - kSegmentSize);

    // ---- index 段（P1M3 新增）----
    inline constexpr int kIndexKeyNotFound     = InSeg(kIndexBase, 0);  // -102000

    static_assert(kIndexKeyNotFound > kIndexBase - kSegmentSize);

    // ---- wal 段（P5M2 新增：WAL 运行期；P5M5 增补环形相关）----
    inline constexpr int kWalKeyTooLong  = InSeg(kWalBase, 0); // -103000  key 超过 kWalKeyMax（Put 拒绝）
    inline constexpr int kWalWriteFailed = InSeg(kWalBase, 1); // -103001  WAL 设备写/落盘失败（区别于数据盘 IO 错）
    inline constexpr int kWalFull           = InSeg(kWalBase, 2); // -103002  WAL 环已满且快照救援无效（响亮的运维信号，非常态错误）
    inline constexpr int kWalInvalidReclaim = InSeg(kWalBase, 3); // -103003  回收边界几何校验不过（内部不变式被破坏；保守失败，head 不动）

    static_assert(kWalInvalidReclaim > kWalBase - kSegmentSize);

    // ---- wal_recovery 段（P5M1 新增：设备超级块校验）----
    inline constexpr int kSuperBlockMagicMismatch      = InSeg(kWalRecoveryBase, 0); // -105000  主备都非本格式/未格式化（魔数或版本不符）
    inline constexpr int kSuperBlockCrcMismatch        = InSeg(kWalRecoveryBase, 1); // -105001  本格式但主备 CRC 均损坏
    inline constexpr int kSuperBlockEngineUuidMismatch = InSeg(kWalRecoveryBase, 2); // -105002
    inline constexpr int kSuperBlockPairMismatch       = InSeg(kWalRecoveryBase, 3); // -105003
    inline constexpr int kSuperBlockDeviceIdMismatch   = InSeg(kWalRecoveryBase, 4); // -105004
    inline constexpr int kSuperBlockDeviceTypeMismatch = InSeg(kWalRecoveryBase, 5); // -105005  设备类型不符（如把 WAL 盘当数据盘）
    inline constexpr int kSuperBlockNoValid            = InSeg(kWalRecoveryBase, 6); // -105006  兜底：主备均无效但无法细分
    inline constexpr int kSuperBlockReadFailed         = InSeg(kWalRecoveryBase, 7); // -105007  主备超级块均读 I/O 失败（区别于"非本格式/损坏"，勿据此重建）
    inline constexpr int kSuperBlockEntropyFailure     = InSeg(kWalRecoveryBase, 8); // -105008  系统熵源不可用，无法生成 UUID，create 中止
    inline constexpr int kSuperBlockSizeMismatch       = InSeg(kWalRecoveryBase, 9); // -105009  持久 block_count 与当前数据设备实际大小不符（设备被缩容）

    // ---- wal_recovery 段续编（P5M6 新增：崩溃恢复，从 10 号起；按运维动作配码——
    //      "查设备 / 查数据 / 报 bug" 三类，细节定位归日志三件套（seq + 偏移 + 原因）。----
    inline constexpr int kWalRecoveryReadFailed = InSeg(kWalRecoveryBase, 10); // -105010  恢复扫描/走读/重灌读 I/O 失败（证据不可得 → 拒开；查设备）
    inline constexpr int kWalRecoveryCorrupted  = InSeg(kWalRecoveryBase, 11); // -105011  WAL 证据矛盾：历史缺页/碎片越容差/帧语义违例/删不存在键/无帧但 covered>0（查数据）
    inline constexpr int kWalRecoveryInvariant  = InSeg(kWalRecoveryBase, 12); // -105012  恢复收尾自检不过（扫描/重建代码自身 bug；报 bug）

    static_assert(kWalRecoveryInvariant > kWalRecoveryBase - kSegmentSize);

    // ---- snapshot 段（P5M4 新增：快照写 + 部署期容量校验；P5M6 增补恢复读侧）----
    inline constexpr int kSnapshotWriteFailed = InSeg(kSnapshotBase, 0); // -106000  写/刷快照设备失败
    inline constexpr int kDeviceTooSmall      = InSeg(kSnapshotBase, 1); // -106001  Open 部署期容量校验不过（设备过小）
    inline constexpr int kSnapshotReadFailed  = InSeg(kSnapshotBase, 2); // -106002  槽头/记录区读 I/O 失败（证据不可得 → 拒开；查设备）
    inline constexpr int kSnapshotCorrupted   = InSeg(kSnapshotBase, 3); // -106003  快照证据矛盾：撞代际/双槽数据皆坏/记录违例（查数据）

    static_assert(kSnapshotCorrupted > kSnapshotBase - kSegmentSize);

    // io 段的具体码随模块产生时补入。
} // namespace cabe::err
#endif // CABE_ERROR_CODE_H