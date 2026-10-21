#include "scene_content.hpp"
#include "eawr/core/text.hpp"

#include "eawr/data/retail_values.hpp"
#include "eawr/presentation/ui/hud_shell.hpp"
#include "eawr/data/ui/text_database.hpp"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <limits>
#include <sstream>

namespace eawr::viewer::galactic {
namespace {

[[nodiscard]] std::string folded(std::string_view value) {
    return core::to_lower(value);
}

[[nodiscard]] std::string trimmed(std::string_view value) {
    return core::trim(value, core::xml_whitespace);
}

[[nodiscard]] float scalar(sim::math::Fixed value) {
    return static_cast<float>(static_cast<double>(value.raw()) / static_cast<double>(sim::math::Fixed::scale));
}

constexpr std::size_t rgba_components = 4;
constexpr std::size_t xyz_components = 3;

class Fields final {
public:
    Fields(const data::Catalog& catalog, std::string_view name, data::Category category) {
        auto effective = catalog.resolve(name, category);
        if (!effective) return;
        for (const std::string& layer : effective.value().chain) {
            if (const auto* definition = catalog.find(layer, category)) layers_.push_back(definition);
        }
    }
    explicit Fields(const data::XmlNode& node) {
        own_.root = node;
        layers_.push_back(&own_);
    }
    [[nodiscard]] const data::XmlNode* node(std::string_view tag) const { return data::retail::single(layers_, tag); }
    [[nodiscard]] std::string text(std::string_view tag) const {
        const auto* value = node(tag);
        return value ? trimmed(value->raw_text) : std::string();
    }
    [[nodiscard]] bool flag(std::string_view tag) const {
        return data::retail::boolean(text(tag)).value_or(false);
    }
    [[nodiscard]] std::optional<float> number(std::string_view tag) const {
        const auto value = data::retail::leading_number(text(tag));
        return value ? std::optional(scalar(value->value)) : std::nullopt;
    }
    [[nodiscard]] std::optional<std::vector<float>> components(std::string_view tag, std::size_t count) const {
        std::string raw = text(tag);
        std::replace(raw.begin(), raw.end(), ',', ' ');
        std::istringstream input(raw);
        std::vector<float> values;
        std::string token;
        while (input >> token) {
            const auto value = data::retail::leading_number(token);
            if (!value || value->trailing_text) return std::nullopt;
            values.push_back(scalar(value->value));
        }
        return count == 0 || values.size() == count ? std::optional(values) : std::nullopt;
    }
private:
    data::Definition own_;
    std::vector<const data::Definition*> layers_;
};

[[nodiscard]] std::string model_path(const std::string& name) {
    if (name.empty()) return {};
    std::string path = folded(name);
    std::replace(path.begin(), path.end(), '\\', '/');
    return path.starts_with("data/") ? path : "data/art/models/" + path;
}

[[nodiscard]] core::Diagnostic failure(std::string message) {
    core::Diagnostic result;
    result.code = "EAWR-GALACTIC-ART-0001";
    result.message = std::move(message);
    return result;
}

} // namespace

std::map<std::string, ProductionArt> load_production_art(
    const data::Catalog& catalog, const data::ui::TextDatabase* text) {
    std::map<std::string, ProductionArt> result;
    std::size_t order{};
    for (const auto& definition : catalog.definitions()) {
        if (!definition.active || !definition.namespace_winner
            || definition.category != data::Category::game_object) continue;
        const Fields fields(catalog, definition.id, data::Category::game_object);
        auto looks = presentation::ui::unit_card_looks(definition.id, &catalog, text, false);
        ProductionArt art;
        art.galactic_model = model_path(fields.text("Galactic_Model_Name"));
        // GHUD-S78: non-buildable bases (including neutral bases) still have display art.
        art.station = fields.flag("Build_Tab_Space_Station") || fields.number("Base_Level").value_or(0.0F) > 0.0F;
        art.sold_sound = fields.text("SFXEvent_Sold_Tactical");
        // GHUD-S90: category flags admit choices; they do not select the row.
        const bool unit_filter = fields.flag("Build_Tab_Land_Units") || fields.flag("Build_Tab_Space_Units")
            || fields.flag("Build_Tab_Special_Structures") || fields.flag("Build_Tab_Heroes");
        const bool outpost = fields.flag("Build_Tab_Outpost");
        const bool station_tab = fields.flag("Build_Tab_Space_Station");
        art.build_tab = unit_filter || outpost || station_tab;
        art.base_filter = !unit_filter && (outpost || station_tab);
        art.name = definition.id; art.icon = looks.icon;
        art.text = looks.name; art.text_id = fields.text("Text_ID"); art.type_order = order++;
        art.encyclopedia = looks.description;
        art.max_health = fields.number("Tactical_Health");
        art.named_hero = fields.flag("Is_Named_Hero");
        if (const auto* rank = fields.node("Ranking_In_Category"))
            if (const auto value = data::retail::leading_number(rank->raw_text))
                art.hero_rank = value->value.trunc_to_integer();
        art.hero = art.named_hero || fields.flag("Is_Generic_Hero") || fields.flag("Is_Special_Weapon_In_Space");
        art.force_sensitive = fields.flag("Is_Force_Sensitive");
        // Shared object content (CARD-17): negative Population_Value usage.
        art.population = looks.population;
        art.encyclopedia_looks = std::move(looks);
        if (const auto* row = fields.node("GUI_Row"))
            if (const auto value = data::retail::leading_number(row->raw_text))
                art.gui_row = value->value.trunc_to_integer();
        result.emplace(definition.id, std::move(art));
    }
    return result;
}

PlanetAbilityMetadata planet_ability_metadata(const data::galactic::Planet& planet,
    const data::ui::TextDatabase* text, std::vector<std::string>& diagnostics) {
    PlanetAbilityMetadata result;
    result.name_key = planet.planet_ability_name;
    result.description_key = planet.planet_ability_description;
    result.icon = planet.planet_ability_icon;
    const auto resolve = [&](const std::string& key, std::string_view tag) {
        if (key.empty()) return std::string{};
        if (text) if (const auto* entry = text->find(key)) return data::ui::to_utf8(entry->value);
        diagnostics.push_back("planet " + planet.name + " unresolved " + std::string(tag) + ": " + key);
        return std::string{};
    };
    result.authored_name = resolve(result.name_key, "Planet_Ability_Name");
    result.authored_description = resolve(result.description_key, "Planet_Ability_Description");
    // GHUD-S95: the debug-build popup reads these distinct fields, even when
    // a mod authors different Planet_Ability_Name/Description values.
    result.title = resolve(planet.describe_advantage, "Describe_Advantage");
    if (!planet.encyclopedia_text.empty())
        result.description = resolve(planet.encyclopedia_text.front(), "Encyclopedia_Text");
    if (!result.icon.empty()) {
        if (planet.describe_advantage.empty()) diagnostics.push_back("planet " + planet.name + " missing Describe_Advantage");
        if (planet.encyclopedia_text.empty()) diagnostics.push_back("planet " + planet.name + " missing Encyclopedia_Text");
    }
    return result;
}

std::string_view to_string(SceneLayer layer) noexcept {
    switch (layer) {
    case SceneLayer::world: return "world";
    case SceneLayer::background: return "background";
    case SceneLayer::midground: return "midground";
    case SceneLayer::midforeground: return "midforeground";
    }
    return "world";
}

void update_affiliation(Affiliation& current, const Affiliation* apparent, bool plain) {
    if (apparent == nullptr) return; // WGP-06: null changes nothing.
    current = *apparent;
    if (plain) current.secondary = {1.0F, 1.0F, 1.0F}; // WGP-05.
    current.present = true;
}

presentation::lighting::Environment galaxy_lighting(Vec3 angles) {
    presentation::lighting::Environment result;
    constexpr float radians = std::numbers::pi_v<float> / 180.0F;
    // WGP-01: ignore X; use the same Y/Z source-angle conversion as the camera
    // flights (R-LIT-01). Matrices are converted once at the renderer boundary.
    result.lights[0] = {presentation::lighting::retail_light_direction(angles.y * radians, angles.z * radians),
                        {1.0F, 1.0F, 1.0F, 0.5F}};
    result.lights[1].color.a = 0.0F;
    result.lights[2].color.a = 0.0F;
    result.ambient = {0.05F, 0.05F, 0.05F, 1.0F};
    result.specular = {0.0F, 0.0F, 0.0F, 1.0F};
    return result;
}

double animation_elapsed(double elapsed, bool paused, bool animate_during_pause) noexcept {
    return paused && !animate_during_pause ? 0.0 : (std::isfinite(elapsed) ? std::max(0.0, elapsed) : 0.0);
}

const Affiliation* affiliation_for_owner(const SceneContent& content, std::optional<PlayerId> owner) {
    if (owner) {
        const auto found = content.factions.find(*owner);
        return found == content.factions.end() ? nullptr : &found->second;
    }
    for (const auto& [id, faction] : content.factions) {
        static_cast<void>(id);
        if (folded(faction.faction) == "neutral") return &faction;
    }
    return nullptr;
}

std::optional<presentation::particles::SystemDefinition> load_galactic_particle(
    const vfs::Vfs& filesystem, const data::Catalog& catalog, const std::string_view type) {
    const Fields particle(catalog, type, data::Category::game_object);
    const auto path = model_path(particle.text("Galactic_Model_Name"));
    if (path.empty()) return {};
    const auto bytes = filesystem.open(path);
    if (!bytes) return {};
    auto system = presentation::particles::load_alo(bytes.value(), path);
    if (!system || system.value().emitters.empty()) return {};
    return std::move(system.value());
}

SceneLayer galactic_particle_layer(const data::Catalog& catalog, const std::string_view type) {
    const Fields fields(catalog, type, data::Category::game_object);
    if (fields.flag("In_Background")) return SceneLayer::background;
    if (fields.flag("In_Midground")) return SceneLayer::midground;
    if (fields.flag("In_Midforeground")) return SceneLayer::midforeground;
    return SceneLayer::world;
}

core::Result<SceneContent> load_scene_content(const vfs::Vfs& filesystem, const data::Catalog& catalog,
    const data::galactic::GalacticData& tables, std::string_view campaign_name) {
    const auto* campaign = tables.campaign(campaign_name);
    if (!campaign) return core::Result<SceneContent>::failure(failure("unknown campaign " + std::string(campaign_name)));
    SceneContent result;
    auto constants = data::load_document(filesystem, "data/xml/gameconstants.xml");
    if (!constants) return core::Result<SceneContent>::failure(constants.error());
    const Fields settings(constants.value().root);
    if (const auto angles = settings.components("Galactic_Zoom_Out_Light_Angle", xyz_components)) {
        result.light_angles = Vec3{(*angles)[0], (*angles)[1], (*angles)[2]};
        result.lighting = galaxy_lighting(*result.light_angles);
    } else {
        return core::Result<SceneContent>::failure(failure("missing or invalid Galactic_Zoom_Out_Light_Angle"));
    }
    result.animate_during_pause = settings.flag("Animate_During_Galactic_Mode_Pause");
    result.side_names = {settings.text("Good_Side_Name"), settings.text("Evil_Side_Name"),
        settings.text("Corrupt_Side_Name")};
    // WGP-19: use the effective authored type and its galactic particle model.
    result.corruption_particle = settings.text("Corruption_Particle_Name");
    if (!result.corruption_particle.empty()) {
        const Fields particle(catalog, result.corruption_particle, data::Category::game_object);
        const auto path = model_path(particle.text("Galactic_Model_Name"));
        if (!path.empty()) {
            const auto bytes = filesystem.open(path);
            if (bytes) {
                auto system = presentation::particles::load_alo(bytes.value(), path);
                if (system && !system.value().emitters.empty()) result.corruption_system = std::move(system.value());
                else result.diagnostics.push_back("corruption particle model could not be decoded");
            } else result.diagnostics.push_back("corruption particle model unavailable");
        } else result.diagnostics.push_back("corruption particle has no galactic model");
    }
    // WGP-47..49: all settings follow the mounted data, including mod values.
    const auto pair = [&](std::string_view tag, std::array<float, 2>& target) {
        if (const auto values = settings.components(tag, 0); values && values->size() >= 2) {
            if ((*values)[0] >= 0 && (*values)[1] >= 0) target = {(*values)[0], (*values)[1]};
            else result.diagnostics.push_back("negative corruption parameter: " + std::string(tag));
        }
    };
    pair("Particle_Brightness_Per_Corruption_Level", result.corruption_parameters.brightness);
    pair("Particle_Scale_Per_Corruption_Level", result.corruption_parameters.scale);
    pair("Particle_Energy_Per_Corruption_Level", result.corruption_parameters.energy);
    auto& style = result.corruption_style;
    style.line_particle = settings.text("Corruption_Particle_Line_Name");
    style.radius = settings.number("Corruption_Line_Radius").value_or(0);
    style.endpoint_offset = settings.number("Corruption_Line_Start_End_Offset").value_or(0);
    style.grow_seconds = settings.number("Corruption_Line_Grow_Seconds").value_or(0);
    style.path_width = settings.number("Corruption_Path_Width").value_or(0);
    style.path_texture = settings.text("Fleet_Hyperspace_Band_Texture_Name");
    if (const auto colour = settings.components("Corruption_Path_Color", 4))
        std::copy(colour->begin(), colour->end(), style.path_colour.begin());
    if (const auto offset = settings.components("Corruption_Path_Offset", 3))
        style.path_offset = {(*offset)[0], (*offset)[1], (*offset)[2]};
    if (!style.line_particle.empty()) {
        const Fields particle(catalog, style.line_particle, data::Category::game_object);
        const auto path = model_path(particle.text("Galactic_Model_Name"));
        if (!path.empty()) if (const auto bytes = filesystem.open(path)) {
            auto system = presentation::particles::load_alo(bytes.value(), path);
            if (system && !system.value().emitters.empty()) result.corruption_line_system = std::move(system.value());
        }
    }
    const Fields fleet(catalog, "Galactic_Fleet", data::Category::game_object);
    result.fleet_model = model_path(fleet.text("Galactic_Model_Name"));
    result.fleet_scale = fleet.number("Scale_Factor").value_or(1.0F);
    result.fleet_layer_z = fleet.number("Layer_Z_Adjust").value_or(0.0F);
    result.fleet_pre_lit = fleet.flag("Pre_Lit");

    PlayerId next_player = 1;
    std::map<std::string, PlayerId> players;
    for (const auto& faction : tables.factions) {
        const Fields fields(catalog, faction.name, data::Category::faction);
        const auto colour = fields.components("Color", rgba_components);
        if (!colour) continue;
        StubPlayer player;
        player.id = next_player++;
        player.faction = faction.name;
        for (std::size_t i = 0; i < 4; ++i) player.colour[i] = static_cast<std::uint8_t>(std::clamp((*colour)[i], 0.0F, 255.0F));
        Affiliation appearance;
        appearance.faction = faction.name;
        appearance.primary = {(*colour)[0] / 255.0F, (*colour)[1] / 255.0F, (*colour)[2] / 255.0F};
        if (const auto font = fields.components("Display_Font_Color", rgba_components)) {
            appearance.secondary = {(*font)[0] / 255.0F, (*font)[1] / 255.0F, (*font)[2] / 255.0F};
        }
        appearance.icon = fields.text("Icon_Name");
        appearance.present = true;
        players.emplace(folded(faction.name), player.id);
        result.factions.emplace(player.id, std::move(appearance));
        result.leader_types.emplace(player.id, faction.faction_leader);
        result.snapshot.players.push_back(std::move(player));
    }
    // WGUI-38/96: unknown overview owners require authored Neutral Color.
    // Reject incomplete content instead of inventing a tint or retaining one.
    if (!affiliation_for_owner(result, std::nullopt)) {
        return core::Result<SceneContent>::failure(failure("campaign has no drawable Neutral faction Color"));
    }
    auto active = players.find(folded(campaign->starting_active_player));
    if (active == players.end()) return core::Result<SceneContent>::failure(failure("campaign has no drawable active faction"));
    result.snapshot.seats = {{1, active->second, true, true}, {2, active->second, true, true}};
    // WGP-42: retain the first case-insensitive home for each faction even
    // when its object is absent; a later home or owned planet is no fallback.
    for (const auto& home : campaign->home_locations) {
        const auto player = players.find(folded(home.faction));
        if (player != players.end()) result.home_types.try_emplace(player->second, home.name);
    }
    std::map<std::string, ObjectId> ids;
    ObjectId next_object = 1;
    auto language = data::ui::load_language_text_database(filesystem, "English");
    for (const std::string& location : campaign->locations) {
        const auto* planet = tables.planet(location);
        // WGS-01a: IDLE-only galaxy art is separate from conquerable locations.
        if (!planet) planet = tables.galaxy_art_object(location);
        if (!planet || !planet->galactic_position) {
            result.diagnostics.push_back("location " + location + " has no planet presentation");
            continue;
        }
        if (ids.contains(folded(location))) continue;
        const Fields fields(catalog, location, data::Category::game_object);
        const auto scale = fields.number("Scale_Factor");
        if (!scale || *scale <= 0.0F) {
            result.diagnostics.push_back("location " + location + " has no positive Scale_Factor");
            continue;
        }
        StubPlanet item;
        item.id = next_object++;
        item.name = location;
        item.ability = planet_ability_metadata(*planet, language ? &language.value() : nullptr, result.diagnostics);
        item.model = model_path(planet->galactic_model_name);
        item.position = {planet->galactic_position->x, planet->galactic_position->y, planet->galactic_position->z};
        item.scale = sim::math::Fixed::from_raw(static_cast<std::int64_t>(std::llround(*scale * static_cast<double>(sim::math::Fixed::scale))));
        // Static owner adapter pending the ownership snapshot: use authored
        // home locations only; do not infer ownership from unit placements.
        for (const auto& home : campaign->home_locations) {
            const auto owner = players.find(folded(home.faction));
            if (folded(home.name) == folded(location) && owner != players.end()) item.owner = owner->second;
        }
        PlanetArt art;
        // WGP-01b: first true layer flag wins.
        if (fields.flag("In_Background")) art.layer = SceneLayer::background;
        else if (fields.flag("In_Midground")) art.layer = SceneLayer::midground;
        else if (fields.flag("In_Midforeground")) art.layer = SceneLayer::midforeground;
        art.loop_idle = fields.flag("Loop_Idle_Anim_00");
        art.show_name = fields.flag("Show_Name");
        art.label = fields.text("Text_ID");
        if (language) {
            if (const auto* entry = language.value().find(art.label)) art.label = data::ui::to_utf8(entry->value);
        }
        if (const auto adjust = fields.components("Name_Adjust", xyz_components)) art.name_adjust = {(*adjust)[0], (*adjust)[1], (*adjust)[2]};
        art.pick_radius = fields.number("Mouse_Collide_Override_Sphere_Radius").value_or(0.0F);
        if (const auto index = fields.number("Zoomed_Terrain_Index"); index && *index >= 0.0F)
            art.terrain_index = static_cast<std::uint32_t>(*index);
        if (art.layer == SceneLayer::world && art.pick_radius <= 0.0F) {
            result.diagnostics.push_back("location " + location
                + " has no positive Mouse_Collide_Override_Sphere_Radius; picking unavailable");
        }
        if (!item.model.empty()) {
            const std::string idle = item.model.substr(0, item.model.find_last_of('.')) + "_idle_00.ala";
            if (filesystem.stat(idle)) art.idle_animation = idle;
        }
        ids.emplace(folded(location), item.id);
        result.art.emplace(item.id, std::move(art));
        result.snapshot.planets.push_back(std::move(item));
    }
    for (const std::string& name : campaign->trade_routes) {
        const auto* route = tables.trade_route(name);
        if (!route) continue;
        const auto a = ids.find(folded(route->point_a));
        const auto b = ids.find(folded(route->point_b));
        if (a != ids.end() && b != ids.end() && a != b) {
            result.route_styles.emplace(next_object, route->visible_line_name.empty() ? "DEFAULT" : route->visible_line_name);
            result.snapshot.routes.push_back({next_object++, a->second, b->second});
        }
    }
    return core::Result<SceneContent>::success(std::move(result));
}

const StubPlanet* initial_home(const SceneContent& content, const PlayerId player) {
    const auto home = content.home_types.find(player);
    if (home == content.home_types.end()) return nullptr;
    const auto object = std::find_if(content.snapshot.planets.begin(), content.snapshot.planets.end(),
        [&](const auto& planet) { return folded(planet.name) == folded(home->second); });
    return object == content.snapshot.planets.end() ? nullptr : &*object;
}

sim::galactic::TypeId home_leader_type(const SceneContent& content, const PlayerId player,
    const std::map<sim::galactic::TypeId, std::string>& type_names) {
    const auto leader = content.leader_types.find(player);
    if (leader == content.leader_types.end() || leader->second.empty()) return 0;
    const auto wanted = folded(leader->second);
    const auto type = std::find_if(type_names.begin(), type_names.end(), [&](const auto& row) {
        return folded(row.second) == wanted;
    });
    return type == type_names.end() ? 0 : type->first;
}

} // namespace eawr::viewer::galactic
