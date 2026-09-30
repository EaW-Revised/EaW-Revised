#pragma once

#include "eawr/core/diagnostic.hpp"

#include <cstddef>
#include <span>
#include <utility>
#include <vector>

namespace eawr::presentation::godot_backend::detail {

class DiagnosticBuffer final {
public:
    static constexpr std::size_t capacity = 64;

    void push(core::Diagnostic diagnostic) {
        if (!diagnostics_.empty() && equivalent(diagnostics_.back(), diagnostic)) return;
        if (diagnostics_.size() == capacity) diagnostics_.erase(diagnostics_.begin());
        diagnostics_.push_back(std::move(diagnostic));
    }

    [[nodiscard]] std::span<const core::Diagnostic> entries() const noexcept {
        return diagnostics_;
    }

private:
    [[nodiscard]] static bool equivalent(
        const core::Diagnostic& left, const core::Diagnostic& right) noexcept {
        return left.code == right.code
            && left.severity == right.severity
            && left.message == right.message
            && left.logical_path == right.logical_path
            && left.line == right.line
            && left.column == right.column
            && left.source_id == right.source_id;
    }

    std::vector<core::Diagnostic> diagnostics_;
};

} // namespace eawr::presentation::godot_backend::detail
