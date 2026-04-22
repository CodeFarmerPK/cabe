/*
 * Project: Cabe
 * Created Time: 2026-04-22
 * Created by: CodeFarmerPK
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace cabe {

// Status 是 Cabe 公开 API 的统一错误类型，替代内部 int32_t 错误码。
//
// 设计原则：
//   - OK 状态零堆分配（code_=kOK，msg_ 为空 string，SSO 不触发堆分配）
//   - 错误状态的短消息走 SSO（≤15 字节无堆分配），长消息才触发堆分配
//   - [[nodiscard]] 加在类上：任何返回 Status 的函数，编译器都要求调用方显式处理
//   - 内部 int32_t 错误码不泄漏给调用方；详细码通过日志记录，Status 只携带语义分类
//   - ToString() 面向调用方，描述"发生了什么"，不含内部码
class [[nodiscard]] Status {
public:
    // 错误分类——面向调用方的语义，不是内部实现分类
    enum class Code : uint8_t {
        kOK                = 0, // 操作成功
        kNotFound          = 1, // key 不存在或已删除
        kInvalidArgument   = 2, // 参数非法（空 key、nullptr out-param 等）
        kIOError           = 3, // 磁盘 I/O 失败
        kCorruption        = 4, // 数据损坏（CRC 不匹配、内部不变式被破坏）
        kResourceExhausted = 5, // 资源耗尽（BufferPool 满、磁盘空间不足）
        kNotSupported      = 6, // 当前状态不支持此操作（Engine 未 Open 等）
    };

    // 默认构造 = OK，零分配
    Status() noexcept = default;

    // ---- 工厂方法 ----

    static Status OK() noexcept { return Status{}; }

    static Status NotFound(std::string_view msg = {}) {
        return Status{Code::kNotFound, msg};
    }
    static Status InvalidArgument(std::string_view msg = {}) {
        return Status{Code::kInvalidArgument, msg};
    }
    static Status IOError(std::string_view msg = {}) {
        return Status{Code::kIOError, msg};
    }
    static Status Corruption(std::string_view msg = {}) {
        return Status{Code::kCorruption, msg};
    }
    static Status ResourceExhausted(std::string_view msg = {}) {
        return Status{Code::kResourceExhausted, msg};
    }
    static Status NotSupported(std::string_view msg = {}) {
        return Status{Code::kNotSupported, msg};
    }

    // ---- 状态查询 ----

    [[nodiscard]] bool ok()                  const noexcept { return code_ == Code::kOK; }
    [[nodiscard]] bool IsNotFound()          const noexcept { return code_ == Code::kNotFound; }
    [[nodiscard]] bool IsInvalidArgument()   const noexcept { return code_ == Code::kInvalidArgument; }
    [[nodiscard]] bool IsIOError()           const noexcept { return code_ == Code::kIOError; }
    [[nodiscard]] bool IsCorruption()        const noexcept { return code_ == Code::kCorruption; }
    [[nodiscard]] bool IsResourceExhausted() const noexcept { return code_ == Code::kResourceExhausted; }
    [[nodiscard]] bool IsNotSupported()      const noexcept { return code_ == Code::kNotSupported; }

    [[nodiscard]] Code code() const noexcept { return code_; }

    // 人类可读描述，用于日志和调试。
    // 格式："OK" | "<CodeName>" | "<CodeName>: <message>"
    // 不含内部 int32_t 错误码（内部码仅出现在日志中）。
    [[nodiscard]] std::string ToString() const {
        const char* code_str = nullptr;
        switch (code_) {
            case Code::kOK:                return "OK";
            case Code::kNotFound:          code_str = "NotFound";          break;
            case Code::kInvalidArgument:   code_str = "InvalidArgument";   break;
            case Code::kIOError:           code_str = "IOError";           break;
            case Code::kCorruption:        code_str = "Corruption";        break;
            case Code::kResourceExhausted: code_str = "ResourceExhausted"; break;
            case Code::kNotSupported:      code_str = "NotSupported";      break;
            default:                       code_str = "Unknown";           break;
        }
        if (msg_.empty()) {
            return std::string{code_str};
        }
        return std::string{code_str} + ": " + msg_;
    }

private:
    Status(Code code, std::string_view msg) : code_(code), msg_(msg) {}

    Code        code_ = Code::kOK;
    std::string msg_;  // 空串：SSO 零分配；短消息：SSO；长消息：堆
};

} // namespace cabe
