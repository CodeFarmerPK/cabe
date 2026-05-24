# P0 — 基础设施 · 设计文档索引

> P0 阶段目标：让项目能 build、跑通本地组合矩阵（持续集成在 M6 推迟，待仓库托管确定后单独立项），
> 工具库齐备，数据 schema 与公共约定全部定型；无业务逻辑。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) 第五节 "P0 — 基础设施"。

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | 项目骨架与构建系统 | [P0M1_skeleton_design.md](P0M1_skeleton_design.md) | 完成稿，待 owner 终审 |
| M2 | `common/structs.h` schema 定型 | [P0M2_schema_design.md](P0M2_schema_design.md) | 完成稿，待 owner 终审 |
| M3 | 错误码段位规划 + Logger stderr 实装 | [P0M3_errorcode_logger_design.md](P0M3_errorcode_logger_design.md) | 完成稿，待 owner 终审 |
| M4 | `util/hash.{h,cpp}` xxh3 接入 | [P0M4_hash_design.md](P0M4_hash_design.md) | 完成稿，待 owner 终审 |
| M5 | 测试与 bench 框架接入 + util/common 覆盖 | [P0M5_test_bench_design.md](P0M5_test_bench_design.md) | 完成稿，待 owner 终审 |
| M6 | 本地内存检测器组合矩阵 + 测试 / 覆盖率脚本（CI 推迟） | [P0M6_test_scripts_design.md](P0M6_test_scripts_design.md) | 完成稿，待 owner 终审 |
| M7 | P0 设计稿固化与状态同步 | `P0_infra_design.md`（阶段收敛文档） | 待撰写 |

## 里程碑依赖

```
       ┌──► M2 ──┐
M1 ──► ├──► M3 ──┤──► M5 ──► M6 ──► M7
       └──► M4 ──┘
```

M2 / M3 / M4 互不依赖，可并行；建议按 M2 → M3 → M4 串行提 PR 便于 review。
M7 在 M6 完成后定稿，收敛全部 P0 决策。

## 命名与目录约定

- 按阶段分目录：`doc/P0/`、`doc/P1/`…
- 里程碑级设计稿：`P<阶段>M<里程碑>_<主题>_design.md`
- 阶段收敛文档：`P<阶段>_infra_design.md`（或同类）
- 详见 [P0M1_skeleton_design.md](P0M1_skeleton_design.md) §13。

## P0 退出条件（DoD）

1. GCC 15+ 与 Clang 20+ 双工具链 build 通过
2. ASAN / TSAN / UBSAN / Release 四档 CI 全绿
3. 工具库（crc32 / hash / cpu_features / util）单测覆盖 ≥ 80%
4. `P0_infra_design.md` review 通过，锁定本阶段所有 schema 决策
5. README 与 ROADMAP 同步
