#pragma once

#include <LocalScribeCore/LocalScribeCore.h>

#include <optional>
#include <string>
#include <utility>
#include <variant>

namespace localscribe {

struct Error {
    ls_status_code_t code{LS_INTERNAL_ERROR};
    std::string message;
};

template <typename T>
class Expected {
public:
    Expected(T value) : value_(std::move(value)) {}
    Expected(Error error) : value_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept
    {
        return std::holds_alternative<T>(value_);
    }

    explicit operator bool() const noexcept { return hasValue(); }

    [[nodiscard]] T &value() { return std::get<T>(value_); }
    [[nodiscard]] const T &value() const { return std::get<T>(value_); }
    [[nodiscard]] T &&takeValue() { return std::get<T>(std::move(value_)); }

    [[nodiscard]] const Error &error() const
    {
        return std::get<Error>(value_);
    }

private:
    std::variant<T, Error> value_;
};

template <>
class Expected<void> {
public:
    Expected() = default;
    Expected(Error error) : error_(std::move(error)) {}

    [[nodiscard]] bool hasValue() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return hasValue(); }
    [[nodiscard]] const Error &error() const { return *error_; }

private:
    std::optional<Error> error_;
};

inline Expected<void> success() { return Expected<void>{}; }

} // namespace localscribe
