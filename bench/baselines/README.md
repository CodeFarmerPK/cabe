# 性能基线归档目录

性能测试原始数据和历史锚点的归档目录。cabe 以 **P6 为历史性能锚点**；P7 / P8 已归档 loop
原始数据但不作性能优劣结论。P9-D22 已覆盖早期“P9 与前一阶段对比”的承诺：P9 只手动归档 SPDK bench 原始
JSON，不作性能优劣结论、不设置性能门槛，VMware 虚拟 NVMe 数据只作为路径样本。

**历史锚点后端 = io_uring**（同步用法、单线程引擎）：自 P6 冻结 sync 的开发/优化与性能基准、
仅保留其正确性回归。P7/P8 在 io_uring 过渡后端上验证 reactor、无锁、多线程与注册缓冲区，
但没有实现深度异步；P9 转向 SPDK 长期路线。数据使用 `run-bench.sh --backend=io_uring` 采集。
详见 doc/P6/README.md D10、doc/P6/P6M3 §6.5（P6M3-D14~D17）。

P9 的 SPDK JSON 与 P6 io_uring 锚点属于不同后端、不同设备形态，只并列保存，不直接据此计算
提升或退化百分比；真实性能比较留到具备合适物理 NVMe 的后续阶段。

## 目录结构

按阶段分子目录归档；文件为 `run-bench.sh` 跑出的 google-benchmark 原生 JSON（含
mean/median/stddev/cv 聚合），命名 `<bench>.<backend>.<compiler>.json`。

- `p6/` —— P6 锚点（由 P6M4 采集，io_uring / g++）：
  - `engine.io_uring.gcc.json` —— 单线程 `Put`/`Get`/`Delete` × WAL 级别 1/2/3/4（12 条）
  - `wal_concurrency.io_uring.gcc.json` —— 多线程 `Wal` 提交吞吐（1/2/4/8 写者）

- `p7/` —— P7 reactor / 多线程阶段原始数据（io_uring / g++）：
  - `engine.io_uring.gcc.json` —— 单线程 Engine 基准
  - `engine_mt.io_uring.gcc.json` —— 多线程 Engine 基准
  - `wal_concurrency.io_uring.gcc.json` —— `Wal` 并发基准

- `p8/` —— P8 零拷贝主路径阶段原始数据（io_uring / g++）：
  - `engine.io_uring.gcc.json` —— 单线程 Engine 基准
  - `engine_mt.io_uring.gcc.json` —— 多线程 Engine 基准
  - `wal_concurrency.io_uring.gcc.json` —— `Wal` 并发基准
  - `engine_value_put.io_uring.gcc.json` —— 非对齐自备内存、对齐自备内存和 Cabe `ValueBuffer` 三类 Put 基准

用 `scripts/run-bench.sh --show=<file>` 可解析成中位数 / 吞吐 / cv 表。P6～P8 数据均来自当时的
loop 设备环境，只作为功能路径和历史样本，不据此给出性能优劣结论。

## 历史

P0 / P1 早期曾在此归档过基线（`p0_utilities.json` / `p1_single_thread.json`），那是设计
思路未定型时所做，已删除、不再作为参考——一律以 P6 基线为准。

策略详见 ROADMAP.md 第六节「性能基线策略注」、doc/P6/README.md（P6M1-D28）与
doc/P6/P6M2_concurrency_audit_design.md §3。
