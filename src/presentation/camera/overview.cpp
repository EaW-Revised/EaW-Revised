#include "eawr/presentation/camera/overview.hpp"

#include <pugixml.hpp>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <span>
#include <string>
#include <string_view>

namespace eawr::presentation::camera {
namespace {

[[nodiscard]] bool ieq(const std::string_view left, const std::string_view right) noexcept {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin(), [](char a, char b) {
        return std::tolower(static_cast<unsigned char>(a)) == std::tolower(static_cast<unsigned char>(b));
    });
}

[[nodiscard]] core::Result<OverviewConstants> failure(std::string message) {
    core::Diagnostic diagnostic;
    diagnostic.code = std::string(diagnostic_codes::invalid_constants);
    diagnostic.message = std::move(message);
    return core::Result<OverviewConstants>::failure(std::move(diagnostic));
}

} // namespace

core::Result<OverviewConstants> load_overview_constants(const XmlSource tactical_cameras, const Mode mode) {
    pugi::xml_document document;
    const auto parsed = document.load_buffer(tactical_cameras.bytes.data(), tactical_cameras.bytes.size(),
                                             pugi::parse_default, pugi::encoding_auto);
    const std::string path(tactical_cameras.logical_path);
    if (!parsed) return failure(path + " could not be parsed: " + parsed.description());
    const std::string_view wanted = definition_name(mode);
    pugi::xml_node definition;
    for (const pugi::xml_node candidate : document.document_element().children()) {
        if (candidate.type() != pugi::node_element || !ieq(candidate.name(), "TacticalCamera")
            || !ieq(candidate.attribute("Name").as_string(), wanted)) continue;
        if (definition) return failure(path + " has duplicate <TacticalCamera Name=\"" + std::string(wanted) + "\">");
        definition = candidate;
    }
    if (!definition) return failure(path + " has no <TacticalCamera Name=\"" + std::string(wanted) + "\">");

    OverviewConstants constants;
    struct Field final {
        std::string_view tag;
        float* value;
    };
    float clicks = static_cast<float>(constants.clicks);
    const std::array fields{
        Field{"Tactical_Overview_Distance", &constants.distance}, Field{"Tactical_Overview_Distance2", &constants.distance2},
        Field{"Tactical_Overview_Pitch", &constants.pitch},       Field{"Tactical_Overview_Pitch2", &constants.pitch2},
        Field{"Tactical_Overview_FOV", &constants.fov},           Field{"Tactical_Overview_FOV2", &constants.fov2},
        Field{"Tactical_Overview_Clicks", &clicks},               Field{"Tactical_Overview_Click_Time", &constants.click_time},
    };
    for (const Field& field : fields) {
        pugi::xml_node found;
        for (const pugi::xml_node child : definition.children()) {
            if (child.type() != pugi::node_element || !ieq(child.name(), field.tag)) continue;
            if (found) return failure(path + " <" + std::string(wanted) + "> <" + std::string(field.tag) + "> is supplied more than once");
            found = child;
        }
        if (!found) continue;
        auto value = parse_scalar(found.text().as_string());
        if (!value) {
            return failure(path + " <" + std::string(wanted) + "> <" + std::string(field.tag)
                           + "> is not a finite decimal number");
        }
        *field.value = value.value();
    }
    if (!(clicks >= 1.0F) || clicks > 1.0e6F || std::floor(clicks) != clicks) {
        return failure(path + " <" + std::string(wanted) + "> <Tactical_Overview_Clicks> must be a whole number of at least one");
    }
    constants.clicks = static_cast<std::uint32_t>(clicks);
    if (!(constants.distance > 0.0F) || !(constants.distance2 > 0.0F) || !(constants.fov > 0.0F)
        || !(constants.fov < 180.0F) || !(constants.fov2 > 0.0F) || !(constants.fov2 < 180.0F)
        || constants.click_time < 0.0F) {
        return failure(path + " <" + std::string(wanted) + "> has an unusable Tactical_Overview value");
    }
    return core::Result<OverviewConstants>::success(constants);
}

void TacticalOverview::enter(const OverviewLevel level) noexcept {
    if (level_ == level) return;
    level_ = level;
    if (level == OverviewLevel::map) yaw_zeroed_ = true;
    if (level == OverviewLevel::off) yaw_zeroed_ = false;
    clicks_ = 0;
    click_timer_ = 0.0;
    last_zoom_out_.reset();
    ++transitions_;
}

bool TacticalOverview::wheel(const float detents, const float live_distance, const float distance_max,
                             const double now_seconds) {
    if (!std::isfinite(detents) || detents == 0.0F) return level_ == OverviewLevel::off;
    const auto notches = static_cast<std::uint32_t>(std::max(1.0F, std::round(std::abs(detents))));
    switch (level_) {
    case OverviewLevel::off:
        if (detents < 0.0F || live_distance < distance_max - 0.5F) {
            if (detents > 0.0F) {
                clicks_ = 0;
                click_timer_ = 0.0;
                last_zoom_out_ = now_seconds;
            }
            return true;
        }
        if (last_zoom_out_ && now_seconds - *last_zoom_out_ < overview_settle_seconds) {
            clicks_ = 0;
            click_timer_ = 0.0;
        } else {
            clicks_ += notches;
        }
        if (clicks_ >= constants_.clicks) enter(OverviewLevel::overview);
        return true;
    case OverviewLevel::overview:
        if (detents < 0.0F) {
            enter(OverviewLevel::off);
        } else if (constants_.distance >= distance_max - 0.5F) {
            clicks_ += notches;
            if (clicks_ >= constants_.clicks) enter(OverviewLevel::map);
        }
        return false;
    case OverviewLevel::map:
        if (detents < 0.0F) enter(OverviewLevel::overview);
        return false;
    }
    return false;
}

void TacticalOverview::toggle() noexcept {
    switch (level_) {
    case OverviewLevel::off: enter(OverviewLevel::overview); break;
    case OverviewLevel::overview: enter(OverviewLevel::map); break;
    case OverviewLevel::map: enter(OverviewLevel::off); break;
    }
}

void TacticalOverview::advance(const double seconds) noexcept {
    if (clicks_ > 0 && std::isfinite(seconds) && seconds > 0.0) click_timer_ += seconds;
    if (click_timer_ > static_cast<double>(constants_.click_time)) {
        clicks_ = 0;
        click_timer_ = 0.0;
        last_zoom_out_.reset();
    }
}

std::optional<float> TacticalOverview::view_yaw(const float tactical_yaw_degrees) const noexcept {
    if (level_ == OverviewLevel::off) return std::nullopt;
    return yaw_zeroed_ ? 0.0F : tactical_yaw_degrees;
}

core::Result<TacticalFrame> TacticalOverview::frame(const TacticalFrame& tactical, const float tactical_yaw_degrees,
                                                    const float map_half_extent) const {
    if (level_ == OverviewLevel::off) return core::Result<TacticalFrame>::success(tactical);
    const bool map = level_ == OverviewLevel::map;
    float distance = map ? constants_.distance2 : constants_.distance;
    // #515: the XML angles are FoC's (horizontal on 4:3); the frame carries the vertical ones.
    auto vertical = vertical_fov_degrees(constants_.fov);
    if (!vertical) return core::Result<TacticalFrame>::failure(vertical.error());
    if (map && std::isfinite(map_half_extent) && map_half_extent > 0.0F) {
        auto vertical2 = vertical_fov_degrees(constants_.fov2);
        if (!vertical2) return core::Result<TacticalFrame>::failure(vertical2.error());
        constexpr float degrees = 3.14159265358979323846F / 180.0F;
        distance = std::min(distance, map_half_extent / std::tan(vertical2.value() * 0.5F * degrees));
    }
    const float pitch = std::min(map ? constants_.pitch2 : constants_.pitch, 89.0F);
    const float yaw = view_yaw(tactical_yaw_degrees).value_or(tactical_yaw_degrees);
    auto eye = eye_position(std::span<const float, 3>(tactical.target), distance, pitch, yaw);
    if (!eye) return core::Result<TacticalFrame>::failure(eye.error());
    TacticalFrame frame = tactical;
    frame.eye = eye.value();
    frame.up = {0.0F, 1.0F, 0.0F};
    frame.vertical_fov_degrees = vertical.value();
    frame.far_plane = std::max(tactical.far_plane, distance * 2.0F);
    return core::Result<TacticalFrame>::success(frame);
}

} // namespace eawr::presentation::camera
