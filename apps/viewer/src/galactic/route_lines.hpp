#pragma once

#include "scene_content.hpp"

namespace eawr::viewer::galactic {

enum class RouteType : std::uint8_t { neutral, enemy, ally, good, evil, corrupt };
[[nodiscard]] std::string_view to_string(RouteType type) noexcept;

struct LineColour final {
    float r{0.5F}, g{0.5F}, b{0.5F}, a{1.0F};
    friend constexpr bool operator==(const LineColour&, const LineColour&) noexcept = default;
};
struct LineStyle final {
    LineColour out, in;
    float width{};
    std::string render_mode{"ADDITIVE"};
    // Descriptor defaults verified by WGP-12 / U-04 boundary research.
    std::string texture{"W_galaxy_line_alpha.tga"};
    std::string spark_texture{"w_galaxy_dot.tga"};
};
struct RouteStyles final {
    std::map<std::string, std::array<LineStyle, 6>> named;
    std::string good_faction, evil_faction;
    std::string fleet_texture, band_texture;
    std::vector<std::string> diagnostics;
};
[[nodiscard]] RouteStyles parse_route_styles(const data::XmlNode& root);
[[nodiscard]] core::Result<RouteStyles> load_route_styles(const vfs::Vfs& filesystem);

// Relations are supplied by the observer adapter; seats never stand in for owners.
struct RouteOwners final {
    std::optional<PlayerId> a, b;
    bool a_neutral{}, b_neutral{}, enemies{}, allied_to_local{};
    std::string a_faction, b_faction;
};
[[nodiscard]] RouteType route_type(const RouteOwners& owners, bool red_green,
                                  std::string_view good, std::string_view evil);

class RouteLine final {
public:
    explicit RouteLine(std::array<LineStyle, 6> styles) : styles_(std::move(styles)) {}
    void recreate(RouteType type);
    bool set_type(RouteType type);
    bool set_zoom(float zoom);
    [[nodiscard]] RouteType type() const noexcept { return type_; }
    [[nodiscard]] float zoom() const noexcept { return zoom_; }
    [[nodiscard]] LineColour colour() const noexcept { return colour_; }
    [[nodiscard]] const LineStyle& style() const noexcept { return styles_[static_cast<std::size_t>(type_)]; }
    [[nodiscard]] bool sparks() const noexcept { return sparks_; }
    [[nodiscard]] bool visible() const noexcept { return current_; }
private:
    std::array<LineStyle, 6> styles_;
    RouteType type_{RouteType::neutral};
    float zoom_{};
    LineColour colour_;
    bool current_{}, sparks_{};
};

inline constexpr std::array<float, 8> route_spark_positions{0.15F, 0.4F, 0.55F, 0.89F, -0.45F, -0.7F, -0.5F, -0.89F};
inline constexpr int route_spark_frames = 50;
inline constexpr float route_spark_size = 3.5F;
inline constexpr float route_spark_rate = 10.0F;
inline constexpr float route_spark_colour_scale = 0.7F;
[[nodiscard]] float spark_position(float signed_position, std::uint64_t services) noexcept;

struct FleetLine final { Vec3 a, b; Rgb colour; float length{}; };
// WGP-17: extent is the destination model box's X extent, not its pick sphere.
[[nodiscard]] std::optional<FleetLine> fleet_line(Vec3 position, bool has_destination_planet,
    std::optional<Vec3> immediate_destination, Vec3 desired, float destination_extent,
    bool locally_visible, Rgb owner_colour);

// WGM-29: a travelling fleet's model turns to its published facing (the hop
// direction); yaw 0 without a planar component (BP-10). WGM-128's model
// quarter-turn here leads with the model's +Y: the retail capture of a
// travelling fleet (#3071) shows the stock model's arrow tips, authored along
// +Y, leading along the hop. Row-major source-basis affine [r0 r1 r2 t].
[[nodiscard]] std::array<float, 12> fleet_model_transform(Vec3 position, Vec3 facing, float scale) noexcept;

} // namespace eawr::viewer::galactic
