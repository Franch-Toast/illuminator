#pragma once

#include <string>
#include <string_view>
#include <variant>
#include <optional>
#include <utility>

namespace illuminator {

enum class StatusCode {
    kOk = 0,
    kCancelled,
    kInvalidArgument,
    kNotFound,
    kAlreadyExists,
    kPermissionDenied,
    kResourceExhausted,
    kInternal,
    kUnimplemented,
    kUnavailable,
    kDataLoss,
};

class Status {
public:
    Status() : code_(StatusCode::kOk) {}

    Status(StatusCode code, std::string message)
        : code_(code), message_(std::move(message)) {}

    static Status Ok() { return Status(); }

    static Status Error(StatusCode code, std::string_view msg) {
        return Status(code, std::string(msg));
    }

    bool ok() const { return code_ == StatusCode::kOk; }
    StatusCode code() const { return code_; }
    const std::string& message() const { return message_; }

    std::string ToString() const {
        if (ok()) return "OK";
        return "Error(" + std::to_string(static_cast<int>(code_)) + "): " + message_;
    }

private:
    StatusCode code_;
    std::string message_;
};

template <typename T>
class StatusOr {
public:
    StatusOr(T value) : data_(std::move(value)) {}
    StatusOr(Status status) : data_(std::move(status)) {}

    bool ok() const { return std::holds_alternative<T>(data_); }

    const T& value() const& { return std::get<T>(data_); }
    T& value() & { return std::get<T>(data_); }
    T&& value() && { return std::get<T>(std::move(data_)); }

    const Status& status() const { return std::get<Status>(data_); }

    const T& operator*() const& { return value(); }
    T& operator*() & { return value(); }

private:
    std::variant<T, Status> data_;
};

}  // namespace illuminator
