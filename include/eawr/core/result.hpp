#pragma once

#include "eawr/core/diagnostic.hpp"

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

namespace eawr::core {

template <typename T>
class Result final {
public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::move(value));
    }

    [[nodiscard]] static Result failure(Diagnostic diagnostic) {
        return Result(std::move(diagnostic));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] T& value() & {
        return std::get<T>(storage_);
    }

    [[nodiscard]] const T& value() const& {
        return std::get<T>(storage_);
    }

    [[nodiscard]] T&& value() && {
        return std::get<T>(std::move(storage_));
    }

    [[nodiscard]] Diagnostic& error() & {
        return std::get<Diagnostic>(storage_);
    }

    [[nodiscard]] const Diagnostic& error() const& {
        return std::get<Diagnostic>(storage_);
    }

private:
    explicit Result(T value) : storage_(std::move(value)) {}
    explicit Result(Diagnostic diagnostic) : storage_(std::move(diagnostic)) {}

    std::variant<T, Diagnostic> storage_;
};

template <>
class Result<void> final {
public:
    [[nodiscard]] static Result success() {
        return Result();
    }

    [[nodiscard]] static Result failure(Diagnostic diagnostic) {
        return Result(std::move(diagnostic));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const {
        if (error_) {
            throw std::logic_error("attempted to read the value of a failed Result<void>");
        }
    }

    [[nodiscard]] Diagnostic& error() & {
        return error_.value();
    }

    [[nodiscard]] const Diagnostic& error() const& {
        return error_.value();
    }

private:
    Result() = default;
    explicit Result(Diagnostic diagnostic) : error_(std::move(diagnostic)) {}

    std::optional<Diagnostic> error_;
};

} // namespace eawr::core
