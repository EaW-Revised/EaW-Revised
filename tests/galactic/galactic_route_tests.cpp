#include "galactic/route_lines.hpp"

#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>
#include <cmath>

using namespace eawr;
using namespace viewer::galactic;

namespace {
data::XmlNode value(std::string name, std::string text) {
    data::XmlNode result;
    result.name = std::move(name);
    result.raw_text = std::move(text);
    return result;
}
data::XmlNode named(std::string name, std::string id, std::vector<data::XmlNode> children) {
    auto result = value(std::move(name), "");
    data::XmlAttribute attribute;
    attribute.name = "Name";
    attribute.value = std::move(id);
    result.attributes.push_back(std::move(attribute));
    result.children = std::move(children);
    return result;
}
bool close(float a, float b) { return std::abs(a - b) < 0.00001F; }

void parser_and_perturbations() {
    auto root = value("TradeRouteLines", "");
    root.children = {named("TradeRouteLine", "Mixed", {
        named("Settings_For_Faction", "Good", {value("Color_Zoomed_Out", "10,20,30,255"),
            value("Color_Zoomed_In", "90,80,70,128"), value("Width", "7"), value("Render_Mode", "additive")}),
        named("Settings_For_Faction", "unrecognized", {value("Width", "TBD"), value("Future", "yes"), value("Width", "")})})};
    auto parsed = parse_route_styles(root);
    assert(parsed.named.contains("MIXED"));
    const auto& good = parsed.named.at("MIXED")[3];
    assert(good.width == 7 && good.render_mode == "ADDITIVE");
    assert(close(good.out.r, 10.0F / 255) && close(good.in.a, 128.0F / 255));
    assert(parsed.diagnostics.size() == 2);
    assert(parsed.named.at("MIXED")[0].width == 0);
    assert(!good.texture.empty() && !good.spark_texture.empty());
    root.children[0].children[0].children[2].raw_text = "12";
    assert(parse_route_styles(root).named.at("MIXED")[3].width == 12);
    root.children[0].children[0].children[1].raw_text = "128, 0 0, 255";
    auto updated = parse_route_styles(root).named.at("MIXED")[3];
    assert(close(updated.in.r, 128.0F / 255) && updated.in.g == 0 && updated.in.b == 0 && updated.in.a == 1);
    root.children[0].children[0].children.push_back(value("Color_Zoomed_In", "64"));
    updated = parse_route_styles(root).named.at("MIXED")[3];
    assert(close(updated.in.r, 64.0F / 255) && updated.in.a == 1);
    root.children[0].children[0].children.push_back(value("Texture_Name", "custom.tga"));
    assert(parse_route_styles(root).named.at("MIXED")[3].texture == "CUSTOM.TGA");
}

void relations_use_faction_identity_and_observer() {
    RouteOwners owners{1, 2, false, false, false, true, "Rebels", "Rebels"};
    assert(route_type(owners, false, "Rebels", "Empire") == RouteType::good);
    assert(route_type(owners, true, "Rebels", "Empire") == RouteType::ally);
    owners.allied_to_local = false;
    assert(route_type(owners, true, "Rebels", "Empire") == RouteType::enemy);
    owners.b_faction = "Empire";
    assert(route_type(owners, false, "Rebels", "Empire") == RouteType::neutral);
    owners.b_faction = "Rebels";
    owners.enemies = true;
    assert(route_type(owners, false, "Rebels", "Empire") == RouteType::neutral);
    owners.enemies = false;
    owners.a_neutral = true;
    assert(route_type(owners, false, "Rebels", "Empire") == RouteType::neutral);
    owners.a_neutral = false;
    owners.a.reset();
    assert(route_type(owners, false, "Rebels", "Empire") == RouteType::neutral);
}

void creation_zoom_and_copies() {
    std::array<LineStyle, 6> styles;
    styles[3].out = {0.2F,0.3F,0.4F,0.8F};
    styles[3].in = {0.8F,0.7F,0.6F,0.4F};
    RouteLine first(styles), second(styles);
    assert(!first.visible());
    first.recreate(RouteType::good);
    assert(first.colour() == styles[3].in && first.sparks());
    assert(!first.set_zoom(0) && first.colour() == styles[3].in);
    assert(first.set_zoom(0.25F) && close(first.colour().r, 0.35F));
    assert(!second.visible() && second.zoom() == 0);
    assert(first.set_zoom(1) && !first.sparks());
    assert(!first.set_type(RouteType::good));
    first.set_type(RouteType::neutral);
    assert(!first.sparks());
    first.set_type(RouteType::good); // Creation at z=1 uses the opposite blend and enables sparks.
    assert(first.colour() == styles[3].out && first.sparks());
    assert(close(spark_position(0.15F, 1), 0.17F));
    assert(close(spark_position(-0.45F, 1), 0.43F));
    assert(close(spark_position(-0.45F, 50), 0.45F));
}

void travelling_line_is_visible_and_shortened() {
    const Vec3 start{0,0,0}, end{100,0,0};
    const Rgb owner{0.2F,0.4F,0.6F};
    auto line = fleet_line(start, true, end, {200,0,0}, 10, true, owner);
    assert(line && line->length == 100 && line->b.x == 90 && line->colour == owner);
    line = fleet_line(start, true, std::nullopt, end, 10, true, owner);
    assert(line && line->b.x == 90);
    assert(!fleet_line(start, false, end, end, 10, true, owner));
    assert(!fleet_line(start, true, end, end, 10, false, owner));
    assert(!fleet_line(start, true, start, end, 10, true, owner));
}

void fleet_model_turns_to_its_facing() {
    // The model's +Y (the stock arrow tips) follows the facing (#3071).
    const auto forward = [](Vec3 facing) {
        const auto m = fleet_model_transform({5, 6, 7}, facing, 2);
        assert(m[3] == 5 && m[7] == 6 && m[11] == 7 && m[10] == 2);
        return Vec3{m[1], m[5], m[9]};
    };
    const auto same = [](Vec3 a, Vec3 b) {
        return std::abs(a.x - b.x) < 1e-5F && std::abs(a.y - b.y) < 1e-5F && std::abs(a.z - b.z) < 1e-5F;
    };
    assert(same(forward({1, 0, 0}), {2, 0, 0}));
    assert(same(forward({0, 1, 0}), {0, 2, 0}));
    assert(same(forward({-3, 0, 0}), {-2, 0, 0}));
    assert(same(forward({0, -0.5F, 0.2F}), {0, -2, 0}));
    assert(same(forward({0, 0, 0}), forward({1, 0, 0}))); // BP-10: no planar facing is yaw 0.
}
}

int main() {
    parser_and_perturbations();
    relations_use_faction_identity_and_observer();
    creation_zoom_and_copies();
    travelling_line_is_visible_and_shortened();
    fleet_model_turns_to_its_facing();
}
