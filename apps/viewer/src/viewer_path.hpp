#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <utility>

namespace eawr::presentation::godot_backend {

// Keeps Godot resource identifiers intact until the FileAccess boundary. Native
// paths remain UTF-8 here and are converted only when the platform filesystem is
// actually used.
class ViewerPath final {
public:
    ViewerPath() = default;
    explicit ViewerPath(std::string value) : value_(std::move(value)) {}
    explicit ViewerPath(const std::string_view value) : value_(value) {}

    [[nodiscard]] const std::string& value() const noexcept { return value_; }

    [[nodiscard]] bool is_godot_resource() const noexcept {
        return value_.starts_with("res://") || value_.starts_with("user://");
    }

    [[nodiscard]] std::filesystem::path native() const {
        return std::filesystem::path(std::u8string(
            reinterpret_cast<const char8_t*>(value_.data()), value_.size()));
    }

private:
    std::string value_;
};

} // namespace eawr::presentation::godot_backend
