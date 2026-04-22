/*
* Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 *
 * Cabe 内部日志接口（当前为 no-op 存根）
 *
 * 日志级别语义：
 *   DEBUG — 正常业务路径的详细跟踪（key not found 属于预期分支，不算错误）
 *   WARN  — 调用方编程错误（传入空 key、重复调用 Open 等）
 *   ERROR — 系统级故障（磁盘 I/O 失败、内存耗尽）
 *   FATAL — 内部不变式被破坏（double-release、CRC 与索引不一致等引擎内部 bug）
 *
 * 接入方式：日志框架就绪后，将下方宏替换为真实实现即可；
 *           engine_api.cpp 等所有调用点自动生效，无需逐处修改。
 *
 * 为何用宏而非函数：
 *   宏在 no-op 时被编译器完全消除（零运行时开销），且可零成本注入
 *   __FILE__ / __LINE__ / __func__ 供未来实现使用。
 */

#pragma once

// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define CABE_LOG_DEBUG(fmt, ...) ((void)0)
#define CABE_LOG_WARN(fmt, ...)  ((void)0)
#define CABE_LOG_ERROR(fmt, ...) ((void)0)
#define CABE_LOG_FATAL(fmt, ...) ((void)0)
// NOLINTEND(cppcoreguidelines-macro-usage)
