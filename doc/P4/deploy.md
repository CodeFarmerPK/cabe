# Cabe io_uring 后端部署文档

> 本文档说明使用 io_uring 后端（`-DCABE_IO_BACKEND=io_uring`）时的系统要求、
> 权限配置和常见问题。

---

## 1. 系统要求

| 项目 | 要求 |
|---|---|
| 操作系统 | Linux（Fedora 43+） |
| 内核版本 | ≥ 6.16 |
| liburing | ≥ 2.9（`setup-dev.sh` 自动安装） |
| 编译器 | GCC 15+ 或 Clang 20+ |

## 2. io_uring 启用检查

Linux 6.6+ 提供了 sysctl 开关控制 io_uring 的可用性：

```bash
cat /proc/sys/kernel/io_uring_disabled
```

| 值 | 含义 | 处理 |
|---|---|---|
| 0 | 启用 | 正常使用 |
| 1 | 受限——需要 `CAP_SYS_ADMIN` 权限 | 以 root 运行或授予权限 |
| 2 | 完全禁用 | `sudo sysctl -w kernel.io_uring_disabled=0` |

如果 `/proc/sys/kernel/io_uring_disabled` 不存在（内核 < 6.6），默认启用。

## 3. 内存锁定限制

io_uring 的 ring 和预注册文件描述符需要锁定内存。如果 `RLIMIT_MEMLOCK` 不够，`io_uring_queue_init` 或 `io_uring_register_files` 可能失败。

检查当前限制：
```bash
ulimit -l    # 单位 KB
```

如果值较小（如 64 KB），调整 `/etc/security/limits.conf`：
```
*    soft    memlock    unlimited
*    hard    memlock    unlimited
```

修改后重新登录生效。

## 4. 推荐设备路径

cabe 的路由公式 `hash(key) % N` 依赖设备顺序。**强烈建议使用持久路径**，避免重启后盘符漂移导致设备顺序错乱：

```cpp
// 推荐：持久路径（不会因重启而变化）
opts.devices.push_back({"/dev/disk/by-id/nvme-SAMSUNG_MZVL2512_xxx"});

// 不推荐：易变路径（重启后编号可能变化）
opts.devices.push_back({"/dev/nvme0n1"});
```

查看可用的持久路径：
```bash
ls -la /dev/disk/by-id/ | grep nvme
```

> **注**：P5 阶段将引入设备超级块机制，在每个设备的第 0 块写入身份标识，
> Open 时自动校验设备顺序。届时即使路径错乱也能检测到并拒绝打开。
> （P5M1 兑现注：已实施——形态为设备**头部独立 8K 双份超级块**（非"第 0 块"数据块；bcache
> 风格，P5-D8），含引擎 UUID + 设备编号 + 三设备双向配对，recover 模式 Open 全部校验、
> 错乱即拒开（`kSuperBlock*` 系列错误码）。本节"按 by-id 配路径"建议依然有效，超级块是其兜底。）

## 5. TSAN 说明

io_uring 后端**不支持** TSAN（线程检测器）测试。

**原因**：io_uring 的提交队列和完成队列通过内存映射与内核共享，TSAN 无法追踪内核侧的内存操作，会产生大量误报。

**处理方式**：
- `run-tests.sh` 已做互斥检查——`--backend=io_uring --tsan` 会被直接拒绝
- TSAN 测试使用 sync 后端即可覆盖 cabe 全部并发逻辑（Engine 层的并发代码与 I/O 后端无关）

```bash
# TSAN 测试（sync 后端）
./scripts/run-tests.sh --backend=sync --tsan --device=/dev/loop0

# io_uring 测试（ASAN / UBSAN 可用）
./scripts/run-tests.sh --backend=io_uring --asan --device=/dev/loop0
./scripts/run-tests.sh --backend=io_uring --ubsan --device=/dev/loop0
```

## 6. 常见问题

### CMake 报 liburing 找不到

```
CMake Error: pkg_check_modules: liburing>=2.9 not found
```

运行 `setup-dev.sh` 安装依赖：
```bash
./scripts/setup-dev.sh
```

### io_uring_queue_init 失败

可能原因：
- io_uring 被 sysctl 禁用（见 §2）
- 内存锁定限制不够（见 §3）
- 权限不足（需要 root 或 `CAP_SYS_ADMIN`）

### 编译时找不到 liburing.h

确认 `liburing-devel` 已安装：
```bash
rpm -q liburing-devel
pkg-config --modversion liburing
```

---

**全文完。**
