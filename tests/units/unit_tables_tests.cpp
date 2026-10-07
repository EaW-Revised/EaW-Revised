#include "unit_tables_support.hpp"
// P2-02 (#65) unit-table contracts. The synthetic catalog, priority sets,
// constants and bone hierarchies are invented here. With EAWR_EAW_GAME_ROOT
// set, the pinned FoC fleet is also loaded read-only from the installation and
// its counts, references and content identity are pinned; nothing from the
// installation is written.

#include "eawr/assets/map.hpp"
#include "eawr/units/unit_tables.hpp"

#include "eawr/data/xml.hpp"
#include "eawr/scene/scene.hpp"
#include "eawr/platform/sim_workers.hpp"
#include "eawr/sim/math/geometry.hpp"
#include "eawr/sim/replay.hpp"
#include "eawr/sim/tactical/damage.hpp"
#include "eawr/sim/tactical/durability.hpp"
#include "eawr/sim/tactical/session.hpp"
#include "eawr/sim/tactical/replay.hpp"
#include "eawr/sim/world.hpp"
#include "eawr/vfs/vfs.hpp"

#include "../../src/units/unit_internal.hpp"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <utility>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace unit_tables_test_support {


int failures{};

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        ++failures;
        std::cerr << "FAIL: " << message << '\n';
    }
}

std::optional<std::string> environment(const char* name) {
#ifdef _WIN32
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
#else
    const char* value = std::getenv(name);
    if (value == nullptr) return std::nullopt;
    std::string result(value);
#endif
    if (result.empty()) return std::nullopt;
    return result;
}

std::int64_t raw(const std::int64_t whole) { return whole * Fixed::scale; }

Vec3 point(const std::int64_t x, const std::int64_t y, const std::int64_t z) {
    return {Fixed::from_raw(x), Fixed::from_raw(y), Fixed::from_raw(z)};
}


void collision_extents() {
    auto wide = model("data/art/models/wide.alo", {bone("Root", -1, {0, 0, 0})});
    eawr::assets::Mesh hull;
    hull.collidable = true;
    hull.bone = 0;
    hull.bounds_min = {-100.0F, -10.0F, -5.0F};
    hull.bounds_max = {100.0F, 10.0F, 5.0F};
    wide.meshes.push_back(hull);
    const auto frames = eawr::units::bind_frames(wide);
    expect(static_cast<bool>(frames), "the wide model's frames bind");
    if (!frames) return;
    const auto one = eawr::units::detail::collision_half_extents(wide, frames.value());
    expect(one && one->first.raw() == raw(100) && one->second.raw() == raw(10), "an X-wide mesh: half extents 100 and 10");
    // A second, Y-long mesh off to the side widens only the axes it reaches past.
    eawr::assets::Mesh mast;
    mast.collidable = true;
    mast.bone = 0;
    mast.bounds_min = {-20.0F, -30.0F, -5.0F};
    mast.bounds_max = {-10.0F, 50.0F, 5.0F};
    wide.meshes.push_back(mast);
    const auto two = eawr::units::detail::collision_half_extents(wide, frames.value());
    expect(two && two->first.raw() == raw(100) && two->second.raw() == raw(40), "two meshes: the union box's half extents");
}


} // namespace

using namespace unit_tables_test_support;

int main() {
    {
        // FW-25: validated inherited flags apply independently of XML class.
        for (const auto& class_name : {"StarBase", "SpaceBuildable", "SpecialStructure", "SecondaryStructure", "Marker"}) {
            TempTree tree;
            auto xml = units_xml("3600", "Fog_Tag_Test");
            xml.insert(xml.rfind("</Objects>"), std::string("<") + class_name + " Name=\"Fog_Test\">"
                "<Variant_Of_Existing_Type>Test_Frigate</Variant_Of_Existing_Type>"
                "<Last_State_Visible_Under_FOW>Yes</Last_State_Visible_Under_FOW>"
                "<Initial_State_Visible_Under_FOW>True</Initial_State_Visible_Under_FOW>"
                "<Faction_Anim_Subindex>Neutral, 2</Faction_Anim_Subindex>"
                "<Faction_Anim_Subindex>neutral, 3</Faction_Anim_Subindex></" + class_name + ">");
            write_fixture(tree.root, xml);
            const std::array mounts{eawr::vfs::MountSpec{"base", tree.root, "data", {}}};
            auto filesystem = eawr::vfs::Vfs::mount(mounts);
            if (!filesystem) { expect(false, "fog flag fixture mounts"); continue; }
            auto catalog = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
            if (!catalog) { expect(false, "fog flag catalog loads"); continue; }
            eawr::units::LoadInput input;
            input.catalog = &catalog.value().catalog;
            input.filesystem = &filesystem.value();
            input.model = [&](const std::string_view path) -> const eawr::assets::Model* {
                static const auto assets = models(0);
                const auto found = assets.find(std::string(path));
                return found == assets.end() ? nullptr : &found->second;
            };
            input.types = {"Fog_Test"};
            auto loaded = eawr::units::load_unit_tables(input);
            expect(loaded && loaded.value().find("Fog_Test")
                && loaded.value().find("Fog_Test")->last_state_visible_under_fow
                && loaded.value().find("Fog_Test")->initial_state_visible_under_fow, "inherited fog flags load for every structure class");
            if (loaded) {
                expect(loaded.value().find("Fog_Test")->neutral_fog_animation_index == 2U,
                    "neutral faction animation uses the first matching validated row");
                auto changed = loaded.value();
                for (auto& type : changed.units) {
                    type.last_state_visible_under_fow = false;
                    type.initial_state_visible_under_fow = false;
                    type.neutral_fog_animation_index = 1U;
                }
                expect(eawr::units::content_identity(changed) == eawr::units::content_identity(loaded.value()),
                    "fog memory flags leave the replay content identity unchanged");
            }
        }
    }
    synthetic_tables();
    object_weapon_defaults();
    squadron_container_health();
    living_collision_admission();
    ship_suitability_content();
    height_adjusted_aim();
    presentation_admission();
    priority_rules();
    content_identity();
    {
        TempTree tree;
        write_fixture(tree.root, units_xml("250", "Select_Test"));
        const auto assets = models(1.0F);
        const auto legacy = load(tree.root, assets);
        write(tree.root / "XML" / "DifficultyAdjustments.xml", R"xml(<Difficulty_Adjustments>
          <Difficulty_Adjustment Name="Easy_Default"><Credit_Multiplier>0.5</Credit_Multiplier></Difficulty_Adjustment>
          <Difficulty_Adjustment Name="Normal_Default"><Credit_Multiplier>1</Credit_Multiplier></Difficulty_Adjustment>
          <Difficulty_Adjustment Name="Hard_Default"><Credit_Multiplier>1.2</Credit_Multiplier></Difficulty_Adjustment>
        </Difficulty_Adjustments>)xml");
        const auto normal = load(tree.root, assets);
        expect(legacy.tables && normal.tables && eawr::units::content_identity(*legacy.tables)
            == eawr::units::content_identity(*normal.tables), "WPR-12 neutral credit rules retain the Normal replay identity");
        for (const auto& [difficulty, value] : {std::pair{"Easy_Default", "0.5"}, std::pair{"Hard_Default", "1.2"}}) {
            const auto alternate = load(tree.root, assets, difficulty);
            expect(alternate.tables && alternate.tables->ai_credit_multiplier == Fixed::from_decimal(value).value(),
                "WPR-12 selected difficulty reads its credit multiplier");
            expect(alternate.tables && normal.tables && eawr::units::content_identity(*alternate.tables)
                != eawr::units::content_identity(*normal.tables), "WPR-12 alternate credit rules bind a different replay identity");
        }
        const auto unknown = load(tree.root, assets, "Unknown_Default");
        expect(unknown.tables && !unknown.tables->ai_credit_multiplier
            && has_row(unknown.tables->unresolved, "Unknown_Default", "Credit_Multiplier"),
            "WPR-12 missing difficulty stays an explicit data gap");
    }
    {
        eawr::units::UnitTables before;
        before.units.emplace_back();
        auto after = before;
        expect(after == before, "structural table equality of a copy");
        after.units.front().tech_level = 1;
        expect(after != before, "structural comparison includes AI tech level");
        expect(eawr::units::content_identity(after) == eawr::units::content_identity(before),
               "structural comparison leaves replay identity unchanged");
        after = before;
        after.units.front().spin_away_time = Fixed::from_raw(Fixed::scale);
        expect(after != before, "structural comparison includes spin-away fields");
        after = before;
        after.units.front().abilities.emplace_back();
        before.units.front().abilities.emplace_back();
        after.units.front().abilities.front().projectile_override = "another_projectile";
        expect(after != before, "structural comparison includes ability projectile override");
    }
    {
        eawr::units::UnitTables defaults;
        defaults.units.emplace_back().capture_point = true;
        defaults.obstacles.emplace_back().id = "capture_candidate";
        auto opted_out = defaults;
        opted_out.obstacles.front().influences_capture = false;
        expect(eawr::units::content_identity(defaults) != eawr::units::content_identity(opted_out),
               "an obstacle capture opt-out changes the pad rules identity");
        opted_out = defaults;
        opted_out.obstacles.front().construction_blocker = false;
        expect(eawr::units::content_identity(defaults) != eawr::units::content_identity(opted_out),
               "an obstacle construction opt-out changes the pad rules identity");
        opted_out = defaults;
        opted_out.obstacles.front().living_projectile_collision = true;
        expect(eawr::units::content_identity(defaults) != eawr::units::content_identity(opted_out),
               "WBP-50: obstacle admission to capture/build queries changes the pad rules identity");
    }
    bind_frame_errors();
    hunt_tables();
    mass_driver_type();
    collision_extents();
    foc_fleet();
    if (failures != 0) {
        std::cerr << failures << " unit table contract(s) failed\n";
        return 1;
    }
    std::cout << "unit table contracts passed\n";
    return 0;
}
