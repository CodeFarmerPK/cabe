# P4 io_uring 后端 bench 总结

> P4 阶段(io_uring 后端 + registered buffer pool)各档 bench 归档对照表。
> 设计稿:[`doc/p4_io_uring_design.md`](../../doc/p4_io_uring_design.md) §15.5 bench 归档计划。

---

## 各档归档时机与含义

| 归档标签 | 时机 | 实测内容 |
|---|---|---|
| `p3-final` | P3 收尾 | sync 后端基线,P4 各档对比的"零点" |
| `p4-pre-fixed` | M3 完成后 | io_uring 朴素路径(无 FIXED 无 register_files) |
| `p4-post-fixed` | M4 完成后 | + `io_uring_register_buffers` + `WRITE_FIXED` / `READ_FIXED` |
| `p4-post-fixed-files` | M5 完成后 | + `io_uring_register_files` + `IOSQE_FIXED_FILE` |
| `p4-post-batch` | **M7 完成后,M9 归档** | + `WriteBlocks` / `ReadBlocks` 批量 API + Engine 多 chunk 接入 |
| ~~`p4-post-modelB`~~ | (不存在) | M8 评估 = 不做,Model B 不实施 |

---

## 已归档基线文件

`bench/baselines/` 下当前 P4 相关:

| 文件 | 标签 | 落地时间 |
|---|---|---|
| `p4-m4-pre-fixed-20260428-231401.{env.txt, json}` | `p4-pre-fixed` | 2026-04-28(M3 完成) |
| `p4-m4-post-fixed-20260429-110036.{env.txt, json}` | `p4-post-fixed` | 2026-04-29(M4 完成) |
| `p4-m5-post-fixed-files-20260514-135857.{env.txt, json}` | `p4-post-fixed-files` | 2026-05-14(M5 完成) |
| `p4-post-batch-*` | **TODO**(M7 已合入但 bench 待跑) | 用户 Linux 端跑完后补入 |

---

## M4 实测数据(已收入)

**bench 命令**:
```bash
taskset -c 0 ./build-bench-io_uring/cabe_bench \
    --benchmark_counters_tabular=true \
    --benchmark_format=json \
    --benchmark_out=p4-m4-post-fixed.json
```

**结果**:
- cpu_time 加速 **16-82%**(small op GUP 消除受益最大)
- **远超设计稿 W4.6 验收门**(≥ 5%)
- 验证 `io_uring_register_buffers` + `*_FIXED` 路径在 1 MiB 块上跳过 GUP / page pin 的红利

**测试环境**:
- OS: Fedora Linux 43 (Server Edition)
- Kernel: 6.17.8-300.fc43.x86_64
- CPU: AMD Ryzen 9 9950X 16-Core(taskset 绑核 0)
- Memory: 16 GiB
- 编译器: gcc (GCC) 15.2.1 20251111

---

## M5 实测数据(已收入)

**改动**:M4 基础上 + `register_files` + `IOSQE_FIXED_FILE`,sqe 用 fd_idx=0 提交,跳过 fdget/fdput。

**结果**:
- cpu_time 落在 ±5% 测试环境噪声内(M5 红利 ~1-3% 难以从噪声中显著分离)
- 功能正确性由 `io_backend_contract_test.cpp` 18 用例 + io_uring_specific_test.cpp 4 用例保证
- M5 价值更多在于"为 P6 reactor 高频提交场景摊销 fdget/fdput",而非 P4 单 NVMe 1 MiB 块场景的可观测加速

---

## M7 批量 API bench(待跑)

**改动**(已合入,M7 ✅):
- `WriteBlocks` / `ReadBlocks` 一次提交 N 个 SQE → `submit_and_wait(N)` →
  `io_uring_for_each_cqe` + `cq_advance(N)`,省 (N-1) 次 syscall
- Engine `Put` / `Get` / `GetIntoVector` 多 chunk 路径按 `bufferPoolCount_` 分批

**bench 归档命令**(用户 Linux 端执行):
```bash
# 准备:loop device 或真实裸盘
sudo ./scripts/mkloop.sh create
export CABE_BENCH_DEVICE=/dev/loop0

# 归档 p4-post-batch
./scripts/run-bench.sh --backend=io_uring --archive p4-post-batch

# 关注 Big value(16 chunks / 16 MiB)路径:
#   BM_CabeEngine_Put / BM_CabeEngine_Get(单线程)
#   BM_CabeEngine_ConcurrentGet(1/2/4/8/16 线程)
```

**期望收益**:
- 大 value Put / Get:syscall 数从 N 次 `submit_and_wait(1)` 降到 1 次
  `submit_and_wait(N)`,~(N-1) 次 syscall 节省
- 实际数据待用户跑完后填入下表:

| metric | p4-post-fixed-files | p4-post-batch | 改善 |
|---|---|---|---|
| Big value Put (16 chunks) cpu_time | TBD | TBD | TBD |
| Big value Get (16 chunks) cpu_time | TBD | TBD | TBD |
| ConcurrentGet 1 thread | TBD | TBD | TBD |
| ConcurrentGet 16 threads total throughput | TBD | TBD | TBD |

---

## M8 评估结论:不做 Model B

详见 [`doc/p4_io_uring_design.md`](../../doc/p4_io_uring_design.md) §13 M8 决策结论。
要点:

1. **架构定位错配**:P4 公开 API 仍是 sync(D3),Model B 内部异步化拿不到真正
   overlap 收益
2. **设备容量已饱和**:1 MiB 块 + 单 NVMe 下 Model A 单线程已逼近带宽极限
3. **Model B 在 P6 路线图上无归宿**:P6 reactor 采用 per-thread ring,直接绕过
   Model B 的 reaper + inflight 表协议
4. **复杂度成本**:~700 行 reaper 协议 + TSAN 不能验

→ 不归档 `p4-post-modelB`。

---

## 跨阶段对照(预留 P5-P9 接口)

P4 各档 bench 在后续阶段的角色:

| 后续阶段 | 与 P4 各档的关系 |
|---|---|
| P5 WAL | WAL 写复用现有 WriteBlock 路径,带 `p4-post-batch` 红利 |
| P6 reactor | 公开 API 异步化;reactor 内部直接 per-reactor ring,**不沿用 Model B**;M7 batch + user_data 编码思路可复用 |
| P7 B+ 树 | 节点 I/O 走 BufferHandle,自动享受 registered buffer + FIXED ops |
| P8 scatter-gather | M7 batch 是 scatter-gather 中间形态;真正零拷贝在 P8 落地 |
| P9 SPDK(可选) | 完全跳过内核 io_uring 路径,在 SyncIoBackend / IoUringIoBackend 之外加 `SpdkIoBackend` |

---

## 维护

新增 P4 bench 归档时:
1. 在 `bench/baselines/` 下落归档(标签 + 时间戳)
2. 更新本文件"已归档基线文件"表
3. 数据显著变化时补"实测数据"段
4. P4 已收尾,不再新增 milestone bench
