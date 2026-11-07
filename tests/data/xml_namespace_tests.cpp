#include "eawr/data/xml.hpp"
#include "eawr/vfs/vfs.hpp"

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>

int main() {
    using namespace eawr::data;
    int failures{};
    const auto expect = [&](bool condition, const char* message) {
        if (!condition) { ++failures; std::cerr << "FAIL: " << message << '\n'; }
    };
    const auto root = std::filesystem::temp_directory_path() /
        ("eawr-xml-namespaces-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(root / "XML");
    std::ofstream(root / "XML/GameObjectFiles.xml") << "<Game_Object_Files><File>objects.xml</File></Game_Object_Files>";
    // Invented identities collide on both the derived object and its variant base.
    std::ofstream(root / "XML/objects.xml") << R"xml(<Objects>
<UpgradeObject Name="Base"><Tactical_Build_Cost_Multiplayer>10</Tactical_Build_Cost_Multiplayer>
<Abilities><Combat_Bonus_Ability Name="Base"><Damage_Bonus_Percentage>.1</Damage_Bonus_Percentage></Combat_Bonus_Ability></Abilities></UpgradeObject>
<UpgradeObject Name="Derived"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type>
<Abilities><Combat_Bonus_Ability Name="Derived"><Variant_Of_Existing_Type>Base</Variant_Of_Existing_Type></Combat_Bonus_Ability></Abilities></UpgradeObject>
<UpgradeObject Name="Later"><Tactical_Build_Cost_Multiplayer>1</Tactical_Build_Cost_Multiplayer></UpgradeObject>
<UpgradeObject Name="Later"><Tactical_Build_Cost_Multiplayer>2</Tactical_Build_Cost_Multiplayer>
<Abilities><Combat_Bonus_Ability Name="Later"/></Abilities></UpgradeObject>
<UpgradeObject Name="Holder"><Abilities><Combat_Bonus_Ability Name="AbilOnly"><Damage_Bonus_Percentage>.1</Damage_Bonus_Percentage></Combat_Bonus_Ability></Abilities></UpgradeObject>
</Objects>)xml";
    const std::array mounts{eawr::vfs::MountSpec{"synthetic", root, "data", {}}};
    auto filesystem = eawr::vfs::Vfs::mount(mounts);
    if (!filesystem) return 2;
    auto loaded = load_catalog(filesystem.value(), Profile::foc);
    if (!loaded) return 2;
    const auto& catalog = loaded.value().catalog;
    // Duplicate Names keep every definition and the FIRST registered one answers a global lookup (PL-20, #5700):
    // the game object precedes the ability nested inside it.
    expect(catalog.find("Derived") && catalog.find("Derived")->category == Category::game_object,
        "legacy find answers the first registered definition");
    for (int repeat = 0; repeat < 2; ++repeat) {
        const auto legacy = catalog.resolve("Derived");
        const auto object = catalog.resolve("derived", Category::game_object);
        const auto ability = catalog.resolve("DERIVED", Category::ability);
        expect(legacy && legacy.value().category == Category::game_object && legacy.value().type_name == "UpgradeObject",
            "legacy cache remains separate");
        expect(object && object.value().type_name == "UpgradeObject" && object.value().chain.size() == 2,
            "game-object variant chain stays in its namespace, including cached resolves");
        if (object) {
            const auto* cost = object.value().value("Tactical_Build_Cost_Multiplayer");
            expect(cost && cost->value.raw_text == "10", "base object's cost is inherited across a colliding name");
        }
        expect(ability && ability.value().type_name == "Combat_Bonus_Ability" && ability.value().chain.size() == 2,
            "ability namespace resolves its own variant base without a false cycle");
    }
    expect(catalog.find("Later", Category::game_object) && catalog.find("Later", Category::game_object)->namespace_winner,
        "namespace winner is retained for a name that is also an ability");
    const auto later = catalog.resolve("Later", Category::game_object);
    expect(later && later.value().value("Tactical_Build_Cost_Multiplayer") &&
        later.value().value("Tactical_Build_Cost_Multiplayer")->value.raw_text == "1",
        "same-namespace duplicates: the first registered definition answers (PL-20)");
    expect(!catalog.find("Derived", Category::hardpoint) && !catalog.resolve("Derived", Category::hardpoint),
        "a namespace miss never falls back to another registry");
    // A legacy override addresses the first registered definition of the name: the game object for "Derived", the
    // ability for the ability-only name.
    const std::array overrides{ValueOverride{"Derived", "Tactical_Build_Cost_Multiplayer", {"7"}},
                               ValueOverride{"AbilOnly", "Damage_Bonus_Percentage", {".5"}}};
    const auto changed = with_overrides(catalog, overrides);
    expect(static_cast<bool>(changed), "legacy ability override succeeds");
    if (changed) {
        const auto object = changed.value().resolve("Derived", Category::game_object);
        const auto ability = changed.value().resolve("Derived", Category::ability);
        const auto only = changed.value().resolve("AbilOnly", Category::ability);
        expect(object && object.value().type_name == "UpgradeObject", "overlay keeps game-object selection");
        expect(object && object.value().value("Tactical_Build_Cost_Multiplayer") &&
            object.value().value("Tactical_Build_Cost_Multiplayer")->value.raw_text == "7",
            "overlay selects the overridden game object for the colliding name");
        expect(ability && ability.value().type_name == "Combat_Bonus_Ability" &&
            !ability.value().value("Tactical_Build_Cost_Multiplayer"),
            "the colliding ability namespace does not take the game-object override");
        expect(only && only.value().value("Damage_Bonus_Percentage") &&
            only.value().value("Damage_Bonus_Percentage")->value.raw_text == ".5",
            "overlay and namespace cache select the overridden ability");
    }
    std::error_code ignored;
    std::filesystem::remove_all(root, ignored);
    return failures ? 1 : 0;
}
