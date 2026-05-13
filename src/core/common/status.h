// ============================================================================
// Illuminator 状态系统 — 错误处理与结果封装
// ============================================================================
//
// 本文件定义了两个核心类，用于统一项目中的错误处理和返回值传递：
//
// 1. StatusCode 枚举 — 定义所有可能的错误码，参考 gRPC 标准状态码设计。
//    常用的错误码包括：
//    - kOk: 操作成功，无错误
//    - kInvalidArgument: 调用方传入了无效参数
//    - kNotFound: 请求的资源（插件、文件、配置项）不存在
//    - kInternal: 内部错误（系统调用失败、内存分配失败等）
//    - kUnimplemented: 功能尚未实现（作为占位符）
//    - kUnavailable: 服务或资源暂时不可用
//    - kDataLoss: 数据丢失或损坏
//
// 2. Status 类 — 封装了错误码和人类可读的错误消息。
//    - ok() 方法用于快速判断操作是否成功
//    - ToString() 方法将状态转换为可读的字符串，方便日志输出
//
// 3. StatusOr<T> 模板类 — 类似 Rust 的 Result<T, E>，可以容纳一个成功值 T
//    或一个错误 Status。使用 std::variant 实现，避免动态内存分配。
//    - 用 ok() 判断是成功还是失败
//    - 成功时用 value() 或 operator*() 获取结果
//    - 失败时用 status() 获取错误详情
//    - 支持移动语义，避免不必要的拷贝
//
// 设计理念：
//   - 禁止使用 C++ 异常，所有错误通过返回值传递，适合性能敏感的观测系统
//   - Status 轻量级（64 位枚举 + 一个 string），栈上传递无额外开销
// ============================================================================

#pragma once

#include <cassert>
#include <string>
#include <string_view>
#include <variant>
#include <optional>
#include <utility>

namespace illuminator {

// ---- 错误码枚举 ----
// 使用 enum class 保证类型安全，避免与其他整数类型混淆
enum class StatusCode {
    kOk = 0,                  // 成功
    kCancelled,               // 操作被取消
    kInvalidArgument,         // 无效参数
    kNotFound,                // 资源未找到
    kAlreadyExists,           // 资源已存在
    kPermissionDenied,        // 权限不足
    kResourceExhausted,       // 资源耗尽 (内存、文件描述符等)
    kInternal,                // 内部错误
    kUnimplemented,           // 功能未实现
    kUnavailable,             // 服务不可用
    kDataLoss,                // 数据丢失/损坏
};

// ---- Status: 错误状态封装 ----
class Status {
public:
    // 默认构造函数：构造一个表示成功的状态
    Status() : code_(StatusCode::kOk) {}

    // 带错误码和消息的构造函数
    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    // 工厂方法：创建一个表示成功的状态对象
    static Status Ok() { return Status(); }

    // 工厂方法：创建一个错误状态
    // 参数 msg 使用 string_view 避免不必要的字符串拷贝
    static Status Error(StatusCode code, std::string_view msg) {
        return Status(code, std::string(msg));
    }

    // 判断当前状态是否为成功
    [[nodiscard]] bool ok() const { return code_ == StatusCode::kOk; }
    // 获取错误码
    [[nodiscard]] StatusCode code() const { return code_; }
    // 获取错误消息（引用以避免拷贝）
    [[nodiscard]] const std::string& message() const { return message_; }

    // 将状态转为可读字符串
    [[nodiscard]] std::string ToString() const {
        if (ok()) return "OK";
        return "Error(" + std::to_string(static_cast<int>(code_)) + "): " + message_;
    }

private:
    StatusCode code_;          // 错误码
    std::string message_;      // 人类可读的错误描述
};

// ---- StatusOr<T>: Result 类型 ----
// 类似 Rust 的 Result<T,E>，一个值要么是成功的 T，要么是错误的 Status。
// 内部使用 std::variant<T, Status> 实现，零额外内存分配。
template <typename T>
class StatusOr {
public:
    // 从成功值构造（支持移动语义）
    StatusOr(T value) : data_(std::move(value)) {}
    // 从错误状态构造
    StatusOr(Status status) : data_(std::move(status)) {}

    // 判断是否为成功状态（即持有 T 类型的值）
    [[nodiscard]] bool ok() const { return std::holds_alternative<T>(data_); }

    // ---- 获取成功值 ----
    // 提供 const 左值、左值、右值引用三个重载，支持各种使用场景

    [[nodiscard]] const T& value() const& {
        assert(ok() && "StatusOr::value() called on error");
        return std::get<T>(data_);
    }
    [[nodiscard]] T& value() & {
        assert(ok() && "StatusOr::value() called on error");
        return std::get<T>(data_);
    }
    [[nodiscard]] T&& value() && {
        assert(ok() && "StatusOr::value() called on error");
        return std::get<T>(std::move(data_));
    }

    // 获取错误状态（仅在失败时调用有意义）
    [[nodiscard]] const Status& status() const { return std::get<Status>(data_); }

    // operator* 简化访问：使 StatusOr 可像指针一样解引用
    [[nodiscard]] const T& operator*() const& { return value(); }
    [[nodiscard]] T& operator*() & { return value(); }

private:
    // variant 内部存储要么是成功的 T，要么是失败的 Status
    std::variant<T, Status> data_;
};

}  // namespace illuminator
