#include "data_test_support.hpp"
#include "../../apps/viewer/src/presentation_constants.hpp"

#include "eawr/data/xml.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace eawr::tests::data_contracts {

int failures = 0;

void expect(const bool condition, const std::string_view message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

} // namespace eawr::tests::data_contracts

using namespace eawr::tests::data_contracts;

int main() {
    namespace pc = eawr::presentation::godot_backend::presentation_constants;
    for (const auto spelling : {"1", "1.0", "01"}) expect(pc::custom_render(spelling) == 1, "numeric beam selector");
    for (const auto spelling : {"2", "2.0", "02"}) expect(pc::custom_render(spelling) == 2, "numeric kite selector");
    for (const auto spelling : {"1.000000001", "1.5", "nan", "inf", "1junk"})
        expect(pc::custom_render(spelling) == 0, "noninteger or invalid selector does not select a laser");
    for (const float depth : {0.0F, 0.25F, 1.0F})
        expect(pc::laser_width(3.0F, 0.0F, depth) == 3.0F, "zero Z scale gives width factor one");
    const auto scratch = std::filesystem::temp_directory_path() / ("eawr-presentation-constants-" +
        std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(scratch / "Data/XML");
    {
        std::ofstream xml(scratch / "Data/XML/GameConstants.xml");
        xml << "<GameConstants><Laser_Beam_Z_Scale_Factor>4</Laser_Beam_Z_Scale_Factor>"
            "<Laser_Kite_Z_Scale_Factor>2</Laser_Kite_Z_Scale_Factor>"
            "<Shield_Flash_Scale>2, 3, 4</Shield_Flash_Scale>"
            "<Shield_Flash_Duration>0.25</Shield_Flash_Duration></GameConstants>";
    }
    const std::array mounts{eawr::vfs::MountSpec{"constants", scratch / "Data", "data", {}}};
    auto mounted = eawr::vfs::Vfs::mount(mounts);
    expect(static_cast<bool>(mounted), "presentation constants scratch VFS mounts");
    if (mounted) {
        const auto lasers = pc::load_lasers(mounted.value());
        const auto flash = pc::load_flash(mounted.value());
        expect(lasers.beam == 4.0F && lasers.kite == 2.0F, "effective XML laser scales");
        expect(flash.scale == std::array<float, 3>{2.0F, 3.0F, 4.0F} && flash.duration == 0.25F,
               "effective XML flash scale and duration");
        for (const auto invalid : {"-1", "nan", "inf", "1junk", "", "1,2"}) {
            const eawr::data::DocumentOverrides override({{"data/xml/gameconstants.xml", "Laser_Beam_Z_Scale_Factor", {invalid}},
                {"data/xml/gameconstants.xml", "Shield_Flash_Duration", {invalid}}});
            expect(pc::load_lasers(mounted.value()).beam == 8.0F, "invalid laser value falls back");
            expect(pc::load_flash(mounted.value()).duration == 0.1, "invalid flash duration falls back");
        }
        for (const auto invalid : {"1,-1,2", "1,nan,2", "1,inf,2", "1,2", "1,2,3,4"}) {
            const eawr::data::DocumentOverrides override({{"data/xml/gameconstants.xml", "Shield_Flash_Scale", {invalid}}});
            expect(pc::load_flash(mounted.value()).scale == std::array<float, 3>{1.0F, 1.1F, 1.25F},
                   "invalid flash RGB falls back atomically");
        }
        const eawr::data::DocumentOverrides missing({{"data/xml/gameconstants.xml", "Laser_Kite_Z_Scale_Factor", {}}});
        expect(pc::load_lasers(mounted.value()).kite == 1.2F, "missing laser tag falls back");
    }
    std::filesystem::remove_all(scratch);
    run_contracts();
    tag_trace_contracts();
    if (failures == 0) std::cout << "data contracts passed\n";
    else std::cerr << failures << " data contract test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
