# 性能基线归档目录

性能基线（锚点）数据的归档目录。cabe 以 **P6 为唯一性能锚点**——后续优化阶段
（P7 / SPDK / 零拷贝完工）相对前一基线对比，其余阶段不做性能基线。

**锚点后端 = io_uring**（同步用法、单线程引擎）：自 P6 冻结 sync 的开发/优化与性能基准、
仅保留其正确性回归，主线与基准转向 io_uring（P7+ 的异步/无锁/多线程在其上兑现，基准须同
后端才可归因）。采集 `run-bench.sh --backend=io_uring`。详见 doc/P6/README.md D10、
doc/P6/P6M3 §6.5（P6M3-D14~D17）。

## 目录结构

按阶段分子目录归档；文件为 `run-bench.sh` 跑出的 google-benchmark 原生 JSON（含
mean/median/stddev/cv 聚合），命名 `<bench>.<backend>.<compiler>.json`。

- `p6/` —— P6 锚点（由 P6M4 采集，io_uring / g++）：
  - `engine.io_uring.gcc.json` —— 单线程 `Put`/`Get`/`Delete` × WAL 级别 1/2/3/4（12 条）
  - `wal_concurrency.io_uring.gcc.json` —— 多线程 `Wal` 提交吞吐（1/2/4/8 写者）

  用 `scripts/run-bench.sh --show=<file>` 可解析成中位数 / 吞吐 / cv 表。

## 历史

P0 / P1 早期曾在此归档过基线（`p0_utilities.json` / `p1_single_thread.json`），那是设计
思路未定型时所做，已删除、不再作为参考——一律以 P6 基线为准。

策略详见 ROADMAP.md 第六节「性能基线策略注」、doc/P6/README.md（P6M1-D28）与
doc/P6/P6M2_concurrency_audit_design.md §3。
