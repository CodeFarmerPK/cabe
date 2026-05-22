/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 *
 * Cabe 内部日志：stderr 最简实现（P0-M3）。纯头 + 宏，无 .cpp。
 *
 * 级别语义：
 *   DEBUG — 正常业务路径的详细跟踪（key not found 属预期分支，不算错误）
 *   INFO  — 正常关键事件（Open/Close 成功、设备就绪等）
 *   WARN  — 调用方编程错误（传入空 key、重复调用 Open 等）
 *   ERROR — 系统级故障（磁盘 I/O 失败、内存耗尽）
 *   FATAL — 内部不变式被破坏（double-release、CRC 与索引不一致等引擎 bug；仅记录，不 abort）
 *
 * 用法：    CABE_LOG_WARN("empty key: len=%zu", n);   // printf 语法，fmt 须为字符串字面量
 * 级别控制：环境变量 CABE_LOG_LEVEL=DEBUG|INFO|WARN|ERROR|FATAL（默认 WARN，进程启动读一次）
 * 输出格式：[LEVEL][tid][file:line] message
 */

#pragma once

#include <cstdio>  // std::fprintf
#include <cstdlib> // std::getenv
#include <cstring> // std::strrchr

#include <unistd.h> // ::gettid

namespace cabe::log {

    enum class Level : int { Debug = 0, Info, Warn, Error, Fatal };

    inline const char *Name(Level lv) noexcept {
        switch (lv) {
            case Level::Debug: return "DEBUG";
            case Level::Info:  return "INFO";
            case Level::Warn:  return "WARN";
            case Level::Error: return "ERROR";
            case Level::Fatal: return "FATAL";
        }
        return "?????"; // enum 越界兜底（防 -Werror=return-type）
    }

    // 首用解析 CABE_LOG_LEVEL（大小写不敏感）；缺省 / 非法值回落到 Warn。
    // 函数内 static：C++11 线程安全 + 首用初始化、SIOF-safe；inline 函数跨 TU 同一实例。
    inline Level Threshold() noexcept {
        static const Level kLevel = []() noexcept -> Level {
            const char *s = std::getenv("CABE_LOG_LEVEL");
            if (s == nullptr) {
                return Level::Warn;
            }
            const auto ieq = [](const char *a, const char *b) noexcept {
                for (; *a != '\0' && *b != '\0'; ++a, ++b) {
                    const char ca = (*a >= 'a' && *a <= 'z') ? static_cast<char>(*a - 32) : *a;
                    const char cb = (*b >= 'a' && *b <= 'z') ? static_cast<char>(*b - 32) : *b;
                    if (ca != cb) {
                        return false;
                    }
                }
                return *a == *b;
            };
            if (ieq(s, "DEBUG")) return Level::Debug;
            if (ieq(s, "INFO"))  return Level::Info;
            if (ieq(s, "WARN"))  return Level::Warn;
            if (ieq(s, "ERROR")) return Level::Error;
            if (ieq(s, "FATAL")) return Level::Fatal;
            return Level::Warn;
        }();
        return kLevel;
    }

    inline bool Enabled(Level lv) noexcept {
        return static_cast<int>(lv) >= static_cast<int>(Threshold());
    }

    inline const char *Basename(const char *path) noexcept {
        const char *slash = std::strrchr(path, '/');
        return (slash != nullptr) ? slash + 1 : path;
    }

} // namespace cabe::log

// 最简纯头宏：整行单次 std::fprintf（stdio 的 FILE 内部锁保证整行原子、多线程不交织）。
// Enabled() 短路：被级别过滤掉的日志连格式化都不做。fmt 须为 printf 字符串字面量（与前缀拼接）。
// 用 C++20 标准 __VA_OPT__（非 GNU ##__VA_ARGS__），-Wpedantic 干净。
// NOLINTBEGIN(cppcoreguidelines-macro-usage)
#define CABE_LOG_AT(lv, fmt, ...)                                                  \
    do {                                                                           \
        if (::cabe::log::Enabled(lv))                                              \
            std::fprintf(stderr, "[%s][%ld][%s:%d] " fmt "\n", ::cabe::log::Name(lv), \
                         static_cast<long>(::gettid()), ::cabe::log::Basename(__FILE__), \
                         __LINE__ __VA_OPT__(, ) __VA_ARGS__);                      \
    } while (0)

#define CABE_LOG_DEBUG(fmt, ...) CABE_LOG_AT(::cabe::log::Level::Debug, fmt __VA_OPT__(, ) __VA_ARGS__)
#define CABE_LOG_INFO(fmt, ...)  CABE_LOG_AT(::cabe::log::Level::Info, fmt __VA_OPT__(, ) __VA_ARGS__)
#define CABE_LOG_WARN(fmt, ...)  CABE_LOG_AT(::cabe::log::Level::Warn, fmt __VA_OPT__(, ) __VA_ARGS__)
#define CABE_LOG_ERROR(fmt, ...) CABE_LOG_AT(::cabe::log::Level::Error, fmt __VA_OPT__(, ) __VA_ARGS__)
#define CABE_LOG_FATAL(fmt, ...) CABE_LOG_AT(::cabe::log::Level::Fatal, fmt __VA_OPT__(, ) __VA_ARGS__)
// NOLINTEND(cppcoreguidelines-macro-usage)