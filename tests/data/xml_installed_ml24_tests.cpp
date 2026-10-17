// ML-24: the stock FoC variants whose blank or TBD values inherit a parent's real value, through
// EffectiveObject::single_text (the reader the battle-audio, minimap and scene readers use).
// Runs against EAWR_EAW_GAME_ROOT; exit 77 (skip) without it.

#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void expect(const bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

std::optional<std::string> environment(const char* name) {
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) != 0 || value == nullptr) return std::nullopt;
    std::string result(value);
    std::free(value);
    return result.empty() ? std::nullopt : std::optional<std::string>(result);
#else
    const char* value = std::getenv(name);
    return value == nullptr || *value == '\0' ? std::nullopt : std::optional<std::string>(value);
#endif
}

struct Row {
    std::string_view object;
    std::string_view tag;
    std::string_view expected;
};

} // namespace

int main() {
    const auto root = environment("EAWR_EAW_GAME_ROOT");
    if (!root) return 77;
    std::vector<eawr::vfs::MountSpec> specs;
    for (const auto& [id, folder] : {std::pair{std::string("expansion"), std::string("corruption")},
                                     std::pair{std::string("base"), std::string("GameData")}}) {
        auto manifest = eawr::vfs::resolve_manifest_mount(id, std::filesystem::path(*root) / folder / "Data");
        expect(static_cast<bool>(manifest), "FoC layer mounts");
        if (!manifest) return 1;
        specs.push_back(std::move(manifest).value().mount);
    }
    auto filesystem = eawr::vfs::Vfs::mount(specs);
    expect(static_cast<bool>(filesystem), "FoC vfs mounts");
    if (!filesystem) return 1;
    auto loaded = eawr::data::load_catalog(filesystem.value(), eawr::data::Profile::foc);
    expect(static_cast<bool>(loaded), "FoC catalog loads");
    if (!loaded) return 1;

    std::vector<Row> rows;
    for (const auto* hero : {"Biggs_XWing", "Hobbie_XWing", "Porkins_XWing"}) {
        rows.push_back({hero, "SFXEvent_Assist_Move", "Unit_Assist_Move_Wedge"});
        rows.push_back({hero, "SFXEvent_Assist_Attack", "Unit_Assist_Attack_Wedge"});
    }
    rows.push_back({"Red_Leader_XWing", "SFXEvent_Assist_Move", "Unit_Assist_Move_X_Wing"});
    rows.push_back({"Red_Leader_XWing", "SFXEvent_Assist_Attack", "Unit_Assist_Attack_X_Wing"});
    for (const auto* hero : {"Rogue_2_XWing", "Rogue_4_XWing", "Rogue_7_XWing", "Rogue_10_XWing", "Rogue_11_XWing"}) {
        rows.push_back({hero, "SFXEvent_Assist_Move", "Unit_Assist_Move_Wedge_Rogue"});
        rows.push_back({hero, "SFXEvent_Assist_Attack", "Unit_Assist_Attack_Wedge_Rogue"});
    }
    rows.push_back({"Millennium_Falcon_Ground_CIN", "SFXEvent_Select", "Unit_Select_Han_Solo"});
    rows.push_back({"Millennium_Falcon_Ground_CIN", "SFXEvent_Move", "Unit_Move_Han_Solo"});
    rows.push_back({"Millennium_Falcon_Ground_CIN", "SFXEvent_Fleet_Move", "Unit_Fleet_Move_Han_Solo"});
    rows.push_back({"Millennium_Falcon_Ground_CIN", "SFXEvent_Attack", "Unit_Attack_Han_Solo"});
    rows.push_back({"Millennium_Falcon_Ground_CIN", "SFXEvent_Guard", "Unit_Guard_Han_Solo"});
    rows.push_back({"Underworld_Vehicle_Transport_Landing_CIN", "SFXEvent_Guard", "Unit_Guard_Han_Solo"});
    rows.push_back({"The_Peacebringer", "No_Colorization_Color", "255, 255, 255, 255"});
    rows.push_back({"Underworld_Vehicle_Transport_Landing_Saleucami_CIN", "LandBehavior", "HIDE_WHEN_FOGGED"});

    for (const auto& row : rows) {
        const std::string label = std::string(row.object) + " " + std::string(row.tag);
        const auto object = loaded.value().catalog.resolve(row.object);
        expect(static_cast<bool>(object), label + ": resolves");
        if (!object) continue;
        expect(object.value().single_text(row.tag) == row.expected, label + ": inherits " + std::string(row.expected));
        // The stock variant's own occurrence is blank (the old value() read saw nothing).
        const auto* own = object.value().value(row.tag);
        expect(own != nullptr && own->value.raw_text != row.expected, label + ": the variant itself authors a placeholder");
    }
    if (failures != 0) {
        std::cerr << failures << " stock ML-24 contract(s) failed\n";
        return 1;
    }
    std::cout << "stock ML-24 inheritance: " << rows.size() << " rows\n";
    return 0;
}
