#pragma once
#include "eawr/presentation/space/space.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// Private declarations shared by the space presentation implementation files.
namespace eawr::presentation::space {

using Rigid = std::array<float, 12>;

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept;
[[nodiscard]] bool declared(const std::optional<std::string>& value) noexcept;
[[nodiscard]] std::string_view kind_name(const assets::ParameterKind kind) noexcept;
[[nodiscard]] bool finite(const assets::Vec3f& value) noexcept;
[[nodiscard]] assets::Vec3f direction(const Rigid& m, const assets::Vec3f v) noexcept;
[[nodiscard]] assets::Vec3f point(const Rigid& m, const assets::Vec3f v) noexcept;
[[nodiscard]] bool proper_rigid(const Rigid& m) noexcept;
[[nodiscard]] std::string hierarchy_problem(const assets::Model& model, const std::int32_t start);
[[nodiscard]] Rigid model_rigid(const assets::Model& model, const std::int32_t start) noexcept;
[[nodiscard]] std::string hidden_bone(const assets::Model& model, const std::int32_t start);
void add_cause(SurfacePlan& surface, const SurfaceStatus cause);
void settle(SurfacePlan& surface);
[[nodiscard]] bool finite4(const assets::Vec4f& value) noexcept;
[[nodiscard]] float dot3(const assets::Vec3f& a, const assets::Vec3f& b) noexcept;

} // namespace eawr::presentation::space
