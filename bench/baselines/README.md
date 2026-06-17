# 性能基线归档目录（占位）

> **占位说明**：本文件仅为保住本目录在 git 中存在（git 不跟踪空目录）。**P6 本身不在此放任何
> 数据**——P6 的 bench 测试只作正确性证明、数字丢弃（见 doc/P6/P6M3 §1.2）。等 **P6M4**（名义
> 挂 P6 的独立测试里程碑）正式采集 + 上传锚点数据时，本占位文件可能被删除，也可能被扩写为正式
> 基线索引（视 M4 设计而定）——届时看后续规划。

## 用途

性能基线（锚点）数据的归档目录。cabe 以 **P6 为唯一性能锚点**——后续优化阶段
（P7 / SPDK / 零拷贝完工）相对前一基线对比，其余阶段不做性能基线。

**锚点后端 = io_uring**（同步用法、单线程引擎）：自 P6 冻结 sync 的开发/优化与性能基准、
仅保留其正确性回归，主线与基准转向 io_uring（P7+ 的异步/无锁/多线程在其上兑现，基准须同
后端才可归因）。采集时 `run-bench.sh --backend=io_uring`。详见 doc/P6/README.md D10、
doc/P6/P6M3 §6.5（P6M3-D14~D17）。

## 历史

P0 / P1 早期曾在此归档过基线（`p0_utilities.json` / `p1_single_thread.json`），那是设计
思路未定型时所做，已删除、不再作为参考。bench 工具代码（`bench/*_bench.cpp`、
`scripts/run-bench.sh`）保留，供 P6 建基线复用。

策略详见 ROADMAP.md 第六节「性能基线策略注」、doc/P6/README.md（P6M1-D28）与
doc/P6/P6M2_concurrency_audit_design.md §3。
