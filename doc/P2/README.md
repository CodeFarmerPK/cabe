# P2 — 公开 API 冻结声明 · 设计文档索引

> P2 阶段目标：审查 P1 已实装的公开接口（Engine / Options / Status），确认能撑到项目完工，
> 然后声明冻结意图（后续尽量不改；如果被迫要改，改了同步更新文档）。
> 阶段总览见根目录 [ROADMAP.md](../../ROADMAP.md) "P2 — 公开 API 契约冻结"。

## 状态

✅ **已实施**（P2M2 收敛通过）

## 范围摘要

- 审查 P1 已实装的公开 API：`Engine::Open / Put / Get / Delete / Close` 签名、`Options` / `Status` 类型
- 错误码空间评估：六段 × 1000 是否够后续阶段（WAL / io_uring / SPDK / 多 device）
- 公开类型 ABI 承诺范围：结构体布局 / 枚举值 / 函数签名分别承诺到什么程度
- API 承诺语义：Put 部分写 / Engine 析构 / Open 幂等等的行为承诺
- 输出一份**公开 API 符号清单 + 冻结声明**文档
- **不做 PoC**：reactor / 多 device / 零拷贝 / recovery 的验证推迟到各自功能实装后

## 里程碑文档清单

| 里程碑 | 主题 | 设计稿 | 状态 |
|---|---|---|---|
| M1 | API 审查 + 冻结声明 | `P2M1_api_freeze_design.md` | ✅ 已锁定（P2M2 收敛） |
| M2 | P2 收敛 | `P2M2_convergence_design.md` | ✅ 已锁定（P2M2 收敛） |

## 里程碑依赖

```
P2M1 ──► P2M2
```

串行：审查冻结 → 收敛。

## 启动条件

1. ✅ P1 全部完成（P1M5 收敛通过）
2. ✅ P2-D1 里程碑划分锁定（2 个里程碑，无 PoC）
3. ⏳ 用 `/grill-with-docs P2M1` 开第一个里程碑的文档设计

## 已知决策点（按里程碑分配）

### P2M1 承接（API 审查 + 冻结声明）

1. **API 版本号方案**：语义化（major.minor.patch）还是单调整数？版本号是否进 Options / Status 字段还是仅文档承诺？
2. **错误码冻结范围**：仅冻结已分配的码值 + 段位划分，还是连段内编号策略一起冻结？
3. **公开类型 ABI 承诺范围**：结构体布局 / 枚举值 / 函数签名分别承诺到什么程度？Options 的 reserved 字段如何扩展？
4. **Engine 析构语义确认**：P1 已实装"未 Close 直接析构 → 自动 Close + 日志警告"——冻结确认还是调整？
5. **Put 部分写语义**：value 写一半 power loss 时的承诺（关系到 P5 WAL 设计）。P2 冻结 API 承诺语义。
6. **Status 类型确认**：P1 已实装薄包装 struct（`int code` + `ok()`）——冻结确认还是加字段？

### 推迟到各自阶段

| 决策 | 推迟到 | 原因 |
|---|---|---|
| Forward-compat PoC 形态 | 各自阶段 | reactor / 多 device / 零拷贝 / recovery 尚未实装 |
| 零拷贝 BufferHandle 接口 | P8 | P8 实装时再定 |
| 多 device PoC | P3+ | 多 device 路由在 P3 IoBackend 抽象后更合适 |

## P2 退出条件概要

1. 公开 API 符号清单文档输出（列清楚哪些类 / 方法 / 错误码是公开的）
2. 冻结声明文档输出（承诺语义 + ABI 范围 + "尽量不改"的意图声明）
3. 错误码空间评估通过（六段 × 1000 够用）
4. P2M2 收敛稿审阅通过 + ROADMAP / README 状态同步

## 命名与目录约定

参见 [doc/P0/README.md](../P0/README.md) — 沿用 `P<阶段>M<里程碑>_<主题>_design.md`。
