# Cabe P0-M6 设计：本地测试脚本与内存检测器组合矩阵

> 本里程碑交付 cabe 的**本地测试基础设施**：`scripts/run-tests.sh`（八格内存检测器
> 与 Release 组合矩阵，2 工具链 × 4 档）、`scripts/run-coverage.sh`（覆盖率报告，需 `gcovr`），
> 并把缺失的 `gcovr` 顺手补到 `setup-dev.sh`。
>
> **重要范围调整**：本里程碑**不接持续集成（CI）工作流**——cabe 目前是实验性 demo、仓库
> 托管尚未确定（owner 拍板）。ROADMAP M6 字面包含的 "CI 工作流"被推迟，对应的退出条件
> "CI 矩阵全绿"改写为"本地四档 × 双工具链全绿"。
>
> **本文为详细设计**；其中脚本片段为设计示意。

---

## 0. 元信息

| 项 | 值 |
|---|---|
| 阶段 / 里程碑 | P0 / M6 |
| 状态 | **完成稿（待 owner 终审）** —— 七项决策经 grill 流程拍板（见 §3） |
| 上游依赖 | M1（`CABE_SANITIZER` 选项就位）、M5（`ctest` 用例集 + `CABE_COVERAGE` 插桩选项就位） |
| 下游依赖本里程碑 | M7（设计稿收敛 + ROADMAP/README 状态同步）、P1+（业务模块复用同一套脚本） |
| 关联约束 | ROADMAP P0/M6 字面范围；ROADMAP D 决策（P4 范围）：TSAN 与 `io_uring` 不兼容 |
| 退出判定 | 本地四档 × 双工具链 = 8 格 `ctest` 全绿；`run-coverage.sh` 跑出 `util/*.cpp` 行覆盖率 ≥ 80% |

---

## 1. 目标与范围

### 1.1 目标

1. 把 `CABE_SANITIZER`（M1 已实装）与 `CABE_BUILD_TESTS`/`CABE_COVERAGE`（M5 已实装）通过本地脚本串起来，让开发者一行命令跑完 `{GCC, Clang} × {ASAN, TSAN, UBSAN, Release}` 八格组合矩阵。
2. 提供独立的覆盖率脚本，跑出 `util/*.cpp` 与 `common/*.h` 的行覆盖率报告。
3. 把覆盖率工具 `gcovr` 加入 `setup-dev.sh` 的必装清单（M5 review 识别的遗留）。

### 1.2 交付范围（本里程碑产出）

1. **`scripts/run-tests.sh`**：测试组合矩阵脚本（八格，详见 §4）
2. **`scripts/run-coverage.sh`**：覆盖率报告脚本（独立，详见 §5）
3. **`scripts/setup-dev.sh`**：在 `REQUIRED_PKGS` 加 `gcovr`（§6）

### 1.3 推迟范围（明确推迟，标注落点）

| 推迟项 | 落点 | 原因 |
|---|---|---|
| **持续集成（CI）工作流** | **不接，待仓库托管确定后单独立项**（不在 P0 路线图剩余里程碑内） | cabe 是实验性 demo，仓库托管未定；owner 拍板砍 CI（见 §3 D-1） |
| `scripts/run-bench.sh`（微基准脚本） | **不在 M6 讨论**，未来独立交付（与 M7 微基准基线归档配套） | owner 明确"分脚本风格"，bench 用独立脚本；M6 聚焦测试与覆盖率 |
| `run-tests.sh` 的 `--backend=` 参数 + "TSAN + `io_uring`"组合阻断 | **P4** | M6 阶段 `io_uring` 后端尚未接入；阻断逻辑无实际作用对象，仅文档预告（见 §7） |
| `CMakePresets.json` | **不做（未来可选）** | 本里程碑用脚本即可达成"一行命令跑完矩阵" |
| `make coverage` CMake custom target | **不做** | 用独立脚本（`run-coverage.sh`）替代；保持"脚本风格"统一 |

---

## 2. 现状盘点（读码结论）

- **`CABE_SANITIZER` 编译开关已就位**（M1，与 ROADMAP 字面 "M6 才实装"的偏差已通过 M1-D1 锁定）。当前可用 `-DCABE_SANITIZER={none|address|thread|undefined}` 直接构建带检测器的库；M6 只剩"用脚本串起多组合"。
- **`CABE_BUILD_TESTS=ON` + `ctest` 用例就位**（M5）。20 个测试，双工具链全绿。
- **`CABE_COVERAGE=ON` 插桩就位**（M5）。GCC 加 `--coverage`、Clang 加 `-fprofile-instr-generate -fcoverage-mapping`；M5 已验 `util/*.cpp` 行覆盖率 ≈ 98%（`crc32.cpp 95.83%` / `cpu_features.cpp 100%` / `hash.cpp 100%`），用裸 `gcov` 算出。
- **`scripts/setup-dev.sh` 已装**：GCC 15 / Clang 21 / `libasan` / `libtsan` / `libubsan` / `cmake` / `ninja-build` / `gtest-devel` / `gmock-devel` / `google-benchmark-devel` / `liburing-devel`。
- **未装**：`gcovr`（M5 review 识别的遗留，G1 决策本里程碑补）。
- **工作区无 `build-tests/`**（脚本第一次跑会创建）；`.gitignore` 由 owner 自管。

---

## 3. 关键决策（owner 已拍板）

下表汇总 grill 流程的七项决策。详细论证见后续各节。

| # | 决策 | 备选 | 理由 | 状态 |
|---|---|---|---|---|
| **M6-D1** | **不接持续集成（CI）工作流**；M6 只交付本地脚本 + 矩阵 | GitHub Actions / GitLab CI / 自托管 | cabe 是实验性 demo，仓库托管未定；接 CI 是浪费 | 锁定（owner 决定） |
| **M6-D2** | 本地矩阵 **2 工具链 × 4 档 = 8 格全跑**（`--all` 默认） | 单工具链 4 档 / 精简档 / 可选切换 | 贴近 ROADMAP 矩阵原意；本地构建+`ctest` 快（M5 实测 0.05s），8 格也才几十秒 | 锁定 |
| **M6-D3** | `run-tests.sh` 接口：`--asan/--tsan/--ubsan/--release/--all` + `--compiler=` + 失败汇总表 + 退出码、默认精简 / `-v` 全量 | 单档参数 / 不带 `--compiler=` / 别的汇总形式 | ROADMAP 字面 + 调试时按工具链单跑的实际需要 | 锁定 |
| **M6-D4** | `build-tests/` 放**源码树内**；`<工具链>-<档>` 子目录；**跑什么清什么**（不动其他格）；跑完**不清理** | 放 `/tmp`、根目录全清、跑完清理 | 跨调用残留逻辑连续（"脚本只对自己跑的格子负责"）；owner 偏好 | 锁定 |
| **M6-D5** | `run-tests.sh` 与 `run-coverage.sh` **分脚本**；`run-bench.sh` 独立（不在 M6 讨论） | 单脚本带 `--coverage` / `--bench` 档 | 一脚本干一件事；覆盖率插桩与检测器同开会污染报告，分脚本更干净 | 锁定 |
| **M6-D6** | `run-coverage.sh` **默认仅打印** 覆盖率数值 + 着色提示；`--strict` 才以 <80% 退出码 1 硬卡 | 永远硬卡 / 永远只打印 | 本地开发者每天跑、被硬卡到烦；保留 `--strict` 让正式场合（如未来若接 CI）可用 | 锁定 |
| **M6-D7** | TSAN + `io_uring` 组合的阻断逻辑：**M6 仅文档预告，代码不动**；P4 接 `io_uring` 时由 `run-tests.sh` 增强 + CMake 加 `FATAL_ERROR` | 现在全留 / 完全不预告 | M6 阶段 `io_uring` 尚未接入，引入 `--backend=` 等参数无意义 | 锁定 |
| **M6-D8** | M6 顺手把 `gcovr` 加入 `setup-dev.sh` 的 `REQUIRED_PKGS` | 仅在 `run-coverage.sh` 自检报错、等 M7 收敛 | `run-coverage.sh` 必须依赖 `gcovr`；不补则脚本一启动就停 | 锁定（**M6 破"不动 setup-dev"边界一次** — 因这事直接关系 M6 自己脚本能不能跑） |

---

## 4. `run-tests.sh` 设计

### 4.1 用法

```
用法: scripts/run-tests.sh [选项]

档选项（互斥）:
  --asan            只跑 ASAN 档
  --tsan            只跑 TSAN 档
  --ubsan           只跑 UBSAN 档
  --release         只跑 Release 档
  --all             跑全部四档（默认）

工具链选项:
  --compiler=NAME   只在指定工具链上跑（g++ / clang++ / all，默认 all）

冗余度:
  -v, --verbose     输出每格完整日志（CMake configure + 构建 + ctest）
                    默认精简：每格一行 PASS/FAIL，失败时自动 dump
                    `ctest --output-on-failure` 到 stderr

其他:
  -h, --help        输出本用法

退出码:
  0  全格 PASS
  1  任一格 FAIL
```

### 4.2 行为细节

**每格的构建命令**（伪代码）：
```bash
cmake -S "$ROOT" -B "build-tests/${compiler_short}-${flavor}" -G Ninja \
      -DCMAKE_CXX_COMPILER="$compiler" \
      -DCABE_BUILD_TESTS=ON \
      ${sanitizer_flag}   # 例如 -DCABE_SANITIZER=address；release 档不加
```
- `compiler_short`：从 `g++` / `clang++` 推导为 `gcc` / `clang`
- `flavor`：`asan` / `tsan` / `ubsan` / `release`
- `sanitizer_flag` 映射：
  - `asan` → `-DCABE_SANITIZER=address`
  - `tsan` → `-DCABE_SANITIZER=thread`
  - `ubsan` → `-DCABE_SANITIZER=undefined`
  - `release` → 不传（保持默认 `none`）；并显式 `-DCMAKE_BUILD_TYPE=Release`

**build 目录策略（M6-D4）**：
- 根目录 `${ROOT}/build-tests/`（**源码树内**）
- 8 个子目录：`gcc-asan` / `gcc-tsan` / `gcc-ubsan` / `gcc-release` / `clang-asan` / `clang-tsan` / `clang-ubsan` / `clang-release`
- **每次跑前**：`rm -rf` 本次要跑的子目录，再 `mkdir` 重建（保证 configure 干净）
- **不清理其他格**：脚本只对自己这次跑的格子负责（不动用户上次跑后留在那里的别的格子）
- **跑完不清理**：失败的格子留在源码树供调试，成功的格子也留着（下一次跑那一档时再被清空重建）
- **提醒**：`.gitignore` 由 owner 自管；落地后请加 `/build-tests/`（以及未来 `/build-bench/`）

**配置与构建失败的处理**：
- `cmake configure` 失败 → 该格记 FAIL，跳到下一格（不 `set -e`）
- `cmake --build` 失败 → 该格记 FAIL，跳过 `ctest`
- `ctest` 失败 → 该格记 FAIL，dump `--output-on-failure` 到 stderr

### 4.3 失败汇总输出

脚本末尾打印汇总（无论全过还是有失败）：

```
==== 汇总 ====
g++     ASAN     : PASS
g++     TSAN     : PASS
g++     UBSAN    : PASS
g++     Release  : PASS
clang++ ASAN     : FAIL   build 目录: /home/cabe/build-tests/clang-asan
clang++ TSAN     : PASS
clang++ UBSAN    : PASS
clang++ Release  : PASS
--------------
1 / 8 失败
```

退出码：`0` 全 PASS / `1` 任一 FAIL。

---

## 5. `run-coverage.sh` 设计

### 5.1 用法

```
用法: scripts/run-coverage.sh [选项]

选项:
  --compiler=NAME   工具链（g++ / clang++，默认 g++）
                    覆盖率工具自动对应：g++ → gcov + gcovr；clang++ → llvm-cov
  --strict          覆盖率 <80% 退出码 1（默认仅打印 + 着色提示）
  -h, --help        输出本用法
```

### 5.2 行为细节

1. **依赖自检**：
   - `--compiler=g++`（默认）：`command -v gcovr` 失败 → 报错 + 提示 `sudo dnf install gcovr`（M6-D8 顺手把 `gcovr` 装到 `setup-dev.sh`，正常环境下不会触发）。
   - `--compiler=clang++`：自检 `llvm-cov` + `llvm-profdata`，缺失则 `exit 3` + 提示 `sudo dnf install llvm`。**`llvm` 刻意未加入 `setup-dev.sh` 的 `REQUIRED_PKGS`**：M6 默认覆盖率工具链是 g++（已覆盖 §10 DoD），clang 路径属于可选；用户主动选 clang 时按自检提示装即可（保持 §6 "破 setup-dev 边界仅 `gcovr` 一次"精神）。
2. **配置 + 构建 + 跑测试**（单格）：
   ```bash
   cmake -S "$ROOT" -B "build-tests/${compiler_short}-coverage" -G Ninja \
         -DCMAKE_CXX_COMPILER="$compiler" \
         -DCABE_BUILD_TESTS=ON \
         -DCABE_COVERAGE=ON
   cmake --build "build-tests/${compiler_short}-coverage"
   ctest --test-dir "build-tests/${compiler_short}-coverage"
   ```
   - build 目录复用 `build-tests/` 根，独立子目录 `gcc-coverage` / `clang-coverage`，不与 `run-tests.sh` 的 8 格冲突
   - 与 `run-tests.sh` 的 D4 策略一致：每次跑前清空对应子目录、跑完不清理
3. **解析报告**：
   - GCC：`gcovr -r "$ROOT" --filter '<root>/util/' --filter '<root>/common/' --exclude '.*_test\.cpp' "$build_dir"`
   - Clang：`llvm-cov report` + 过滤 `util/` `common/`
4. **打印总览**：
   ```
   ==== 覆盖率（GCC + gcovr）====
   util/crc32.cpp        : 95.83% (24 行)
   util/cpu_features.cpp : 100.00% (15 行)
   util/hash.cpp         : 100.00% (6 行)
   ...
   --------------
   util + common 总行覆盖率: 97.5%   ≥ 80% ✓
   ```
   - ≥ 80%：绿色 ✓
   - < 80%：黄色警示（不退出码失败，除非 `--strict`）

### 5.3 覆盖率门槛（M6-D6）

- **默认**：仅打印数值 + 着色提示；不影响退出码（成功完成即 `0`）
- **`--strict`**：覆盖率 < 80% 时退出码 `1`，用于"正式验收"场合（未来若接 CI 也可用）
- 临界值 80% 与 ROADMAP P0 退出条件对齐

---

## 6. `setup-dev.sh` 改动（M6-D8）

仅在 `REQUIRED_PKGS` 数组里**加一行**：

```bash
REQUIRED_PKGS=(
    gcc
    gcc-c++
    clang
    libasan
    libtsan
    libubsan
    cmake
    make
    ninja-build
    pkgconf-pkg-config
    git
    tar
    kernel-headers
    util-linux
    gtest-devel
    gmock-devel
    google-benchmark-devel
    liburing
    liburing-devel
    gcovr                    # ← M6 新增：scripts/run-coverage.sh 的依赖
)
```

不动 `DEV_EXTRA_PKGS`、io_uring 检查段、版本声明段等。

> 本里程碑**破"不动 setup-dev"边界一次**，理由：`gcovr` 直接关系 M6 自己交付的脚本（`run-coverage.sh`）能否开箱跑。其他 `setup-dev` 相关调整（如 README 依赖列表同步）仍归 **M7** 收敛。
>
> **特别说明**：`llvm`（提供 `llvm-cov` / `llvm-profdata`，clang 覆盖率路径所需）**未**加入 `REQUIRED_PKGS`。默认覆盖率工具链是 g++（§10 DoD 已用 g++ 路径验过 98.3%），clang 路径属于可选；用户用 `--compiler=clang++` 时由脚本的依赖自检（§5.2-1）按设计 `exit 3` 并提示安装。若后续决策要把 clang 覆盖率纳入默认强制路径，再补 setup-dev（不在 M6 范围）。

---

## 7. TSAN 与 `io_uring` 不兼容的提前留预（M6-D7）

**背景**：ROADMAP D 决策（P4 范围）+ P4 设计稿已锁定：`io_uring` 的 SQ/CQ 是用户态与内核共享内存，TSAN 看不到内核侧 store，会产生大量误报；故 TSAN 与 `io_uring` 必须互斥（CMake 层硬阻断 + 脚本层拒绝）。

**M6 阶段**：`io_uring` 后端尚未接入（P4 才接），`run-tests.sh --tsan` 实际跑的是 sync 后端（`CABE_IO_BACKEND` 默认 `sync`），**不会真触发不兼容**。

**本里程碑处理**：仅做**文档预告**，代码不动——

- 本设计稿（即本节）记录"P4 接入 `io_uring` 时需要的脚本/CMake 改造"。
- `run-tests.sh` 头部注释加一行：
  ```bash
  # 注：当前 M6 阶段 io_uring 后端尚未接入（P4 才接）；
  # P4 起本脚本需补 --backend= 参数，并阻断 TSAN + io_uring 组合（与 CMake 层 FATAL_ERROR 配合）。
  ```
- **不**预先引入 `--backend=` 参数（当前无实际后端可切，徒增混淆）。

---

## 8. 与 ROADMAP / 决策一致性核对

| ROADMAP M6 / D 决策 | 本设计 | 状态 |
|---|---|---|
| CMake `CABE_SANITIZER` 选项实装 | M1 已完成（M1-D1） | ✅ |
| `scripts/run-tests.sh`：`--asan/--tsan/--ubsan/--release/--all`，带失败汇总 | §4 | ✅ |
| 本地四档矩阵跑通 | §4 + 双工具链（M6-D2） | ✅ |
| **持续集成（CI）工作流（GitHub Actions 或同等）** | **推迟、不接** | ⚠️ **与 ROADMAP 字面有出入**（M6-D1，owner 锁定；M7 收敛时同步更新 ROADMAP P0 状态） |
| ROADMAP 退出："本地四档全绿 + CI 矩阵全绿" | 改写为"本地四档 × 双工具链全绿"（§10） | ⚠️ 同上 |
| ROADMAP D（P4 范围）：TSAN 与 `io_uring` 不兼容 | §7 文档预告（M6-D7） | ✅ |
| M5 未做：`gcovr` 加入 `setup-dev.sh` | §6（M6-D8） | ✅ |

---

## 9. 风险与权衡

| 风险 | 说明 | 缓解 |
|---|---|---|
| `build-tests/` 在源码树内污染 `git status` | 8 个子目录跑完留在那里，`git status` 显示一片 `??` | `.gitignore` 加 `/build-tests/`（owner 自管；§4.2 提醒） |
| 跨调用残留累积（M6-D4 选 Y 的代价） | 先跑 `--asan` 再 `--tsan`，残留 4 格在源码树内 | 跑 `--all` 会重建全 8 格，相当于"重置"；或手动 `rm -rf build-tests/` |
| `gcovr` 与 `setup-dev` 包管理 | Fedora 包名假设 `gcovr` 存在 | Fedora 43 仓库标配；若包名变动（极少见）M7 时再调 |
| 不接 CI 的中长期风险 | 本地 8 格全绿不等于"任何环境全绿" | 仓库托管确定后单独立项接 CI（不属 P0 范围）；本地脚本作为 CI 基础未来可直接复用 |
| 双工具链 8 格本地耗时 | M5 实测 `ctest` 0.05s + 构建几秒；8 格总耗时几十秒，可接受 | 未来 cabe 代码规模上升后可加 `--jobs=N` 并行；M6 不预实装 |
| `--strict` 默认关 → 覆盖率漂移可能被忽略 | 开发者不主动 `--strict` 跑就不会被卡 | 文档强调"M7 收敛时手动跑一次 `--strict` 确认 ≥80%" |

---

## 10. 退出条件（DoD）与验证步骤

1. **`scripts/run-tests.sh --all`** 在工作区双工具链下 **8 / 8 PASS**，退出码 0。
2. **`scripts/run-tests.sh --asan --compiler=g++`** 只跑 `gcc-asan` 一格，PASS，退出码 0；`build-tests/gcc-asan/` 保留。
3. **`scripts/run-coverage.sh`** 跑出 `util` + `common` 行覆盖率报告，**≥ 80%**（M5 已验 util ≈98%，common 待此脚本汇总验证）；`--strict` 在当前条件下退出码 0。
4. **`scripts/setup-dev.sh`**（本地干净环境）能装上 `gcovr`；`command -v gcovr` 找到。
5. **失败汇总与 build 目录路径**：人为破坏某文件（例如改 `crc32_test.cpp` 让一个测试失败），`run-tests.sh --asan` 输出含 `FAIL ... build 目录: /home/cabe/build-tests/gcc-asan`；恢复后 PASS。
6. **`-v` / `-h`** 行为符合 §4.1。

> 本里程碑**不含**持续集成验证（M6-D1 不接 CI）。

---

## 11. 对下游里程碑的接口承诺

| 里程碑 / 阶段 | M6 提供的接入点 |
|---|---|
| **M7** | 设计稿收敛时同步更新：① ROADMAP P0/M6 的"CI 工作流"字面改为"推迟、待仓库托管确定"；② P0 退出条件中"CI 矩阵全绿"改为"本地 8 格全绿 + `run-coverage.sh --strict` 通过"；③ README 依赖列表（若有 `gcovr` 相关条目）同步。`bench/baselines/p0_utilities.json` 归档时复用 `build-tests/` 的同名风格（`build-bench/` 已被 owner 预留） |
| **P1+ 业务模块** | 复用 `scripts/run-tests.sh`：新模块加测试到 `test/<module>/` 即被 `--all` 矩阵自动覆盖（前提是按 P0M5 的 `cabe_add_test` 模式注册） |
| **P4（`io_uring` 接入）** | 按 §7 文档预告改造 `run-tests.sh`：加 `--backend={sync,io_uring,spdk}` 参数；脚本层阻断 `--tsan` 与 `--backend=io_uring` 组合；CMake 层同步加 `FATAL_ERROR` 阻断 |
| **未来接 CI**（不在 P0 路线图） | `run-tests.sh` 与 `run-coverage.sh --strict` 可直接在 CI 容器（Fedora 43 + `setup-dev.sh --ci`）中调用，无需重写 |
