// Shell anchor contracts (rule UI-L2, UI-03 #170). The synthetic shell is
// built in memory; with EAWR_EAW_GAME_ROOT set, the FoC tactical shell is read
// with the ALO reader and its anchors are pinned to design section 1.3.

#include "eawr/data/ui/command_bar.hpp"
#include "eawr/data/ui/shell_anchors.hpp"

#include "ui_test_support.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {
using namespace eawr;
using data::ui::ReferenceRect;
using data::ui::ShellAnchor;

void expect(const bool condition, const std::string& message) {
    test::ui::expect(condition, message.c_str());
}

bool near(const double actual, const double expected, const double tolerance) {
    return std::fabs(actual - expected) <= tolerance;
}

assets::Bone bone(std::string name, const std::int32_t parent, const float x, const float y, const float z) {
    assets::Bone result;
    result.name = std::move(name);
    result.parent = parent;
    result.relative_transform = {1.0F, 0.0F, 0.0F, x, 0.0F, 1.0F, 0.0F, y, 0.0F, 0.0F, 1.0F, z};
    return result;
}

assets::Submesh quad(const float x0, const float y0, const float x1, const float y1, const float z) {
    assets::Submesh result;
    result.shader = "alDefault.fx";
    for (const auto& [x, y] : {std::pair{x0, y0}, std::pair{x1, y0}, std::pair{x1, y1}, std::pair{x0, y1}}) {
        assets::Vertex vertex;
        vertex.position = {x, y, z};
        result.vertices.push_back(vertex);
    }
    return result;
}

assets::Mesh mesh(std::string name, const std::int32_t bone_index, std::vector<assets::Submesh> submeshes) {
    assets::Mesh result;
    result.name = std::move(name);
    result.bone = bone_index;
    result.submeshes = std::move(submeshes);
    return result;
}

bool rect_near(const ReferenceRect& actual, const ReferenceRect& expected, const double tolerance) {
    return near(actual.x, expected.x, tolerance) && near(actual.y, expected.y, tolerance) &&
           near(actual.width, expected.width, tolerance) && near(actual.height, expected.height, tolerance);
}

std::size_t count_code(const std::vector<core::Diagnostic>& diagnostics, const std::string_view code) {
    return static_cast<std::size_t>(std::count_if(diagnostics.begin(), diagnostics.end(),
        [&](const core::Diagnostic& diagnostic) { return diagnostic.code == code; }));
}

data::ui::CommandBarComponent component(std::string name, const data::ui::ComponentType type) {
    data::ui::CommandBarComponent result;
    result.alt = data::ui::split_alt(name);
    result.name = std::move(name);
    result.type = type;
    return result;
}

void shell_anchor_synthetic() {
    assets::Model model;
    model.source.logical_path = "data/art/models/i_synthetic.alo";
    model.bones.push_back(bone("Root", -1, 0.0F, 0.0F, 0.0F));
    model.bones.push_back(bone("panel", 0, 100.0F, 50.0F, 0.0F));
    model.bones.push_back(bone("b_button", 1, 10.0F, 5.0F, 2.0F));
    model.bones.push_back(bone("Empire_Faceplate_ALT0", 0, 0.0F, 0.0F, -1.0F));
    model.bones.push_back(bone("Rebel_Faceplate_ALT1", 0, 0.0F, 0.0F, -1.0F));
    assets::Bone spun = bone("spun", 0, 300.0F, 0.0F, 0.0F);
    spun.relative_transform[0] = 0.0F;
    spun.relative_transform[1] = 1.0F;
    spun.relative_transform[4] = -1.0F;
    spun.relative_transform[5] = 0.0F;
    model.bones.push_back(spun);

    model.meshes.push_back(mesh("b_button", 2, {quad(-12.0F, -12.0F, 12.0F, 12.0F, 0.0F)}));
    assets::Submesh faceplate = quad(-1.0F, -1.0F, 1077.0F, 283.0F, 0.0F);
    faceplate.shader = "MeshAlpha.fx";
    faceplate.parameters.push_back({"BaseTexture", assets::ParameterKind::texture, std::string("i_dash.tga")});
    model.meshes.push_back(mesh("Empire_Faceplate_ALT0", 3, {faceplate}));
    model.meshes.push_back(mesh("Rebel_Faceplate_ALT1", 4, {quad(-1.0F, -8.0F, 1077.0F, 283.0F, 0.0F)}));
    // Two submeshes: the extents span both.
    model.meshes.push_back(mesh("Text_Planet_tactical", 1,
        {quad(-16.0F, 178.0F, 50.0F, 200.0F, 0.0F), quad(100.0F, 178.0F, 173.84F, 200.0F, 0.0F)}));
    model.meshes.push_back(mesh("empty", 1, {}));
    model.meshes.push_back(mesh("floating", -1, {quad(0.0F, 0.0F, 10.0F, 10.0F, 0.0F)}));
    model.meshes.push_back(mesh("spun", 5, {quad(0.0F, 0.0F, 10.0F, 20.0F, 0.0F)}));
    model.meshes.push_back(mesh("B_BUTTON", 1, {quad(0.0F, 0.0F, 1.0F, 1.0F, 0.0F)}));

    const auto load = data::ui::shell_anchors(model);
    const auto& shell = load.shell;
    namespace codes = data::ui::diagnostic_codes;
    expect(shell.model_path() == "data/art/models/i_synthetic.alo", "shell keeps its model path");
    expect(shell.anchors().size() == 7U, "every mesh with vertices has an anchor");
    expect(count_code(load.diagnostics, codes::shell_mesh_empty) == 1U, "a mesh without vertices is reported");
    expect(count_code(load.diagnostics, codes::shell_mesh_unbound) == 1U, "a mesh without a bone is reported");
    expect(count_code(load.diagnostics, codes::shell_bone_transform) == 0U, "rotation is supported without a warning");
    expect(count_code(load.diagnostics, codes::shell_mesh_duplicate) == 1U, "a repeated mesh name is reported");

    const ShellAnchor* button = shell.find("B_Button");
    expect(button && button->rect == ReferenceRect{98.0F, 43.0F, 24.0F, 24.0F},
           "rect = bone translation summed up the parent chain plus vertex extents");
    expect(button && button->z_min == 2.0F && button->bone == "b_button", "depth and bone name are kept");
    expect(button && button->origin && button->origin->x == 110.0F && button->origin->y == 55.0F,
           "the origin is the bone's model-space translation");
    const ShellAnchor* empire = shell.find("Empire_Faceplate_ALT0");
    expect(empire && empire->rect == ReferenceRect{-1.0F, -1.0F, 1078.0F, 284.0F} && empire->alt.variant == 0U &&
               empire->alt.base == "Empire_Faceplate",
           "faceplate rect and ALT variant");
    expect(empire && empire->shader == "MeshAlpha.fx" && empire->base_texture == "i_dash.tga",
           "shader and base texture of the first submesh");
    const ShellAnchor* planet = shell.find("Text_Planet_tactical");
    expect(planet && rect_near(planet->rect, {84.0F, 228.0F, 189.84F, 22.0F}, 1.0e-3),
           "extents span every submesh");
    const ShellAnchor* floating = shell.find("floating");
    expect(floating && floating->rect == ReferenceRect{0.0F, 0.0F, 10.0F, 10.0F} && !floating->origin,
           "unbound mesh uses its vertices and has no origin");
    const ShellAnchor* rotated = shell.find("spun");
    expect(rotated && rotated->rect == ReferenceRect{300.0F, -10.0F, 20.0F, 10.0F},
           "the full bone transform applies (UI-L2)");

    const auto empire_view = shell.for_variant(data::ui::alt_variant::empire);
    const auto rebel_view = shell.for_variant(data::ui::alt_variant::rebel);
    const auto has = [](const std::vector<const ShellAnchor*>& view, const std::string_view name) {
        return std::any_of(view.begin(), view.end(), [&](const ShellAnchor* anchor) { return anchor->name == name; });
    };
    expect(empire_view.size() == 6U && has(empire_view, "Empire_Faceplate_ALT0") &&
               !has(empire_view, "Rebel_Faceplate_ALT1") && has(empire_view, "b_button"),
           "Empire view: unsuffixed meshes plus ALT0");
    expect(rebel_view.size() == 6U && has(rebel_view, "Rebel_Faceplate_ALT1") &&
               !has(rebel_view, "Empire_Faceplate_ALT0"),
           "Rebel view: unsuffixed meshes plus ALT1");

    data::ui::CommandBarCatalog catalog;
    catalog.add(component("b_button", data::ui::ComponentType::button));
    catalog.add(component("text_planet_TACTICAL", data::ui::ComponentType::text_button));
    catalog.add(component("unused", data::ui::ComponentType::icon));
    const auto bound = data::ui::bind_components(shell, catalog);
    const auto with_component = std::count_if(bound.begin(), bound.end(),
        [](const data::ui::BoundAnchor& entry) { return entry.component != nullptr; });
    expect(bound.size() == 7U && with_component == 3, "anchors bind to components by name, case-insensitively");
}

void shell_anchor_corpus() {
    auto filesystem = test::ui::foc_corpus("shell anchor");
    if (!filesystem) return;
    auto catalog = data::ui::load_command_bar(*filesystem);
    expect(static_cast<bool>(catalog), "FoC command bar loads for the shell");
    if (!catalog) return;
    const auto* shell_component = catalog.value().catalog.shell_for_model("i_tactical_controls.alo");
    expect(shell_component != nullptr, "the catalogue names the tactical shell model");
    auto loaded = data::ui::load_shell_anchors(*filesystem, "i_tactical_controls.alo");
    expect(static_cast<bool>(loaded), "i_tactical_controls.alo loads with the ALO reader");
    if (!loaded) {
        std::cerr << "  " << core::format_diagnostic(loaded.error()) << '\n';
        return;
    }
    const auto& shell = loaded.value().shell;
    for (const auto& diagnostic : loaded.value().diagnostics) {
        std::cerr << "  " << core::format_diagnostic(diagnostic) << '\n';
    }
    const auto bound = data::ui::bind_components(shell, catalog.value().catalog);
    const auto with_component = std::count_if(bound.begin(), bound.end(),
        [](const data::ui::BoundAnchor& entry) { return entry.component != nullptr; });
    std::cout << "shell anchor corpus: " << shell.anchors().size() << " anchors, " << with_component
              << " bound to components, " << loaded.value().diagnostics.size() << " diagnostics\n";
    expect(shell.anchors().size() == 153U, "i_tactical_controls.alo has 153 meshes");
    expect(loaded.value().diagnostics.empty(), "the tactical shell anchors without diagnostics");
    expect(with_component == 144, "144 meshes are components; 9 are decorative");
    // #349: the engine centres button art on the bone, which is not always the
    // mesh centre (the help button's mesh also spans the droid).
    const ShellAnchor* help = shell.find("b_droid_help_tactical");
    expect(help && help->origin && near(help->origin->x, 19.0, 1.0e-3) && near(help->origin->y, 255.0, 1.0e-3),
           "b_droid_help_tactical's bone sits at (19, 255), below its mesh centre");

    const auto rect_of = [&](const std::string_view name) -> std::optional<ReferenceRect> {
        const ShellAnchor* anchor = shell.find(name);
        expect(anchor != nullptr, "tactical shell has mesh " + std::string(name));
        return anchor ? std::optional<ReferenceRect>(anchor->rect) : std::nullopt;
    };
    const auto check = [&](const std::string_view name, const ReferenceRect expected, const double tolerance,
                           const std::string_view what) {
        const auto rect = rect_of(name);
        expect(rect && rect_near(*rect, expected, tolerance), std::string(what) + ": " + std::string(name));
    };
    const auto check_size = [&](const std::string_view name, const float width, const float height,
                                const double tolerance, const std::string_view what) {
        const auto rect = rect_of(name);
        expect(rect && near(rect->width, width, tolerance) && near(rect->height, height, tolerance),
               std::string(what) + ": " + std::string(name));
    };
    const auto numbered = [](const std::string_view stem, const int index) {
        return std::string(stem) + (index < 10 ? "0" : "") + std::to_string(index);
    };

    // Design section 1.3, row by row.
    check("Empire_Faceplate_ALT0", {-1.0F, -1.0F, 1078.0F, 284.0F}, 0.0, "faceplate -1, -1, 1078, 284");
    check("Rebel_Faceplate_ALT1", {-1.0F, -1.0F, 1078.0F, 284.0F}, 0.0, "faceplate -1, -1, 1078, 284");
    // The Underworld faceplate reaches 7 units lower than the design row says.
    check("Underworld_Faceplate_ALT2", {-1.0F, -8.0F, 1078.0F, 291.0F}, 0.0, "Underworld faceplate -1, -8, 1078, 291");
    const ShellAnchor* empire = shell.find("Empire_Faceplate_ALT0");
    expect(empire && empire->shader == "MeshAlpha.fx" &&
               empire->base_texture == "i_galactic_dashboard_skirmish.tga",
           "Empire faceplate is MeshAlpha with i_galactic_dashboard_skirmish");
    check("radar", {14.5F, 10.5F, 175.0F, 175.0F}, 0.0, "minimap 14.5, 10.5, 175, 175");
    const ShellAnchor* radar = shell.find("radar");
    expect(radar && radar->shader == "MeshAdditive.fx", "minimap scan lines are MeshAdditive");

    std::set<float> card_columns;
    std::set<float> card_rows;
    float card_left = 1.0e9F;
    float card_right = -1.0e9F;
    for (int i = 0; i < 24; ++i) {
        const auto rect = rect_of(numbered("s_select_", i));
        if (!rect) continue;
        expect(near(rect->width, 50.0, 0.0) && near(rect->height, 50.0, 0.0), "unit card 50 x 50: " + numbered("s_select_", i));
        card_columns.insert(rect->x);
        card_rows.insert(rect->y);
        card_left = std::min(card_left, rect->x);
        card_right = std::max(card_right, rect->right());
        check_size(numbered("s_health_", i), 50.0F, 8.0F, 1.0, "health bar 50 x 8");
        check_size(numbered("s_shield_", i), 50.0F, 8.0F, 1.0, "shield bar 50 x 8");
        check_size(numbered("special_button_", i), 24.0F, 25.0F, 1.0e-3, "ability button 24 x 25");
    }
    expect(card_columns.size() == 12U && card_rows.size() == 2U, "unit cards: 2 rows x 12");
    expect(card_left == 355.0F && card_right == 1010.0F, "unit cards span x 355...1010");
    for (int i = 0; i < 12; ++i) check_size(numbered("special_border_", i), 54.0F, 119.0F, 0.0, "ability border 54 x 119");
    for (int i = 0; i < 6; ++i) check_size(numbered("c_button", i), 34.0F, 34.0F, 0.0, "order button 34 x 34");
    check("b_option_t", {205.0F, 7.0F, 24.0F, 24.0F}, 0.0, "options 205, 7, 24, 24");
    check("b_play_pause_t", {2.0F, 206.5F, 34.0F, 35.0F}, 0.0, "pause 2, 206.5, 34 x 35");
    const auto fast_forward = rect_of("b_fast_forward_t");
    expect(fast_forward && fast_forward->x == 36.5F && fast_forward->y == 206.5F, "fast forward at 36.5, 206.5");
    // The design rounds the planet-name rect to whole units.
    check("Text_Planet_tactical", {84.0F, 228.0F, 190.0F, 22.0F}, 0.5, "planet name 84, 228, 190, 22");
    for (const std::string_view name : {"Text_Credits_tactical", "Text_Planetary_Pop", "Text_Tactical_Tech"}) {
        const auto rect = rect_of(name);
        expect(rect && near(rect->height, 20.0, 1.0) && near(rect->y, 206.0, 1.0), "about 20 high at y 206: " + std::string(name));
    }
    for (const std::string_view name : {"b_retreat", "b_reinforcement", "b_special_weapon", "b_special_weapon2"}) {
        const auto rect = rect_of(name);
        expect(rect && rect->x == 200.0F && rect->width == 36.0F && rect->height == 36.0F, "36 x 36 at x 200: " + std::string(name));
    }
    // The superweapon switch meshes are not at x 200 as the design row groups them.
    for (const std::string_view name : {"deathstar_switch", "pb_switch", "sl_switch"}) {
        const auto rect = rect_of(name);
        expect(rect && near(rect->x, 350.04, 0.01) && near(rect->width, 49.92, 0.01), "switch at x 350, 50 wide: " + std::string(name));
    }

    const auto empire_view = shell.for_variant(data::ui::alt_variant::empire);
    const auto underworld_view = shell.for_variant(data::ui::alt_variant::underworld);
    expect(empire_view.size() == 149U && underworld_view.size() == 149U,
           "ALT variants: 147 shared meshes plus a faceplate and a help droid per variant");
}

void mod_shell_corpus() {
    auto mods = test::ui::mod_corpora("shell anchors");
    if (mods.empty()) return;
    auto retail = test::ui::foc_corpus("mod shell diagnostic baseline");
    if (!retail) return;
    for (const auto& mod : mods) {
        auto catalog = data::ui::load_command_bar(mod.filesystem);
        expect(static_cast<bool>(catalog), mod.name + " command bar loads");
        if (!catalog) continue;
        std::size_t shells = 0;
        std::size_t inherited_diagnostics = 0;
        for (const auto& component : catalog.value().catalog.components()) {
            if (component.type != data::ui::ComponentType::shell) continue;
            const auto model = component.text(data::ui::Field::model_name);
            auto shell = data::ui::load_shell_anchors(mod.filesystem, model);
            expect(static_cast<bool>(shell), mod.name + " shell " + std::string(model) + " loads through its chain");
            if (!shell) {
                std::cerr << core::format_diagnostic(shell.error()) << '\n';
                continue;
            }
            // G11 already reports rotated hero-frame bones in the retail shell.
            // Compare the complete diagnostic, including source, with FoC so an
            // inherited warning is not mistaken for a mod-chain regression.
            auto baseline = data::ui::load_shell_anchors(*retail, model);
            std::vector<std::string> expected;
            if (baseline) {
                for (const auto& diagnostic : baseline.value().diagnostics)
                    expected.push_back(core::format_diagnostic(diagnostic));
            }
            std::vector<std::string> actual;
            for (const auto& diagnostic : shell.value().diagnostics)
                actual.push_back(core::format_diagnostic(diagnostic));
            expect(actual == expected, mod.name + " shell has no new diagnostics: " + std::string(model));
            if (actual != expected) for (const auto& diagnostic : actual) std::cerr << diagnostic << '\n';
            inherited_diagnostics += actual.size();
            ++shells;
        }
        expect(shells == 18U, mod.name + " loads all 18 surveyed shells");
        std::cout << "mod shell corpus " << mod.name << ": " << shells << " shells, "
                  << inherited_diagnostics << " inherited FoC diagnostics, no new diagnostics\n";
    }
}

} // namespace

void shell_anchor_contracts() {
    shell_anchor_synthetic();
    shell_anchor_corpus();
    mod_shell_corpus();
}
