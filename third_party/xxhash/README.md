# xxHash（vendored / 内嵌）

本目录内嵌 xxHash 的单头文件 `xxhash.h`，供 Cabe 的**路由 hash**（`util/hash`，XXH3 64-bit）使用。

## 为何内嵌（而非系统库）
路由 hash 的稳定性是**数据正确性**问题：`hash(key) % N` 决定一个 key 的数据落在哪个 device，
一旦 hash 输出因库版本/环境变化，同一 key 会路由到不同 device，**已写入的数据将再也读不到**。
据 ROADMAP D6「路由 hash v2.0 前冻结」，钉死版本内嵌、不随发行版浮动。
设计依据见 `doc/P0/P0M4_hash_design.md` §3 决策-1（已 owner 终审）。

## 版本信息
- 上游：https://github.com/Cyan4973/xxHash
- 文件：`xxhash.h`（single-header）
- 版本 tag：`v0.8.2`
- 原始 URL：https://raw.githubusercontent.com/Cyan4973/xxHash/v0.8.2/xxhash.h
- sha256：`be275e9db21a503c37f24683cdb4908f2370a3e35ab96e02c4ea73dc8e399c43`
- 许可：BSD 2-Clause（声明见 `xxhash.h` 头部）

## 升级须知
升级版本前**必须**确认 `XXH3_64bits` 对既有 key 的输出不变（否则等同 v2.0 破坏性变更，会使既有数据路由漂移）。
升级须走 review，并同步更新本文件的版本 tag 与 sha256。
