#include "../support/check.hpp"
#include "eawr/vfs/vfs.hpp"
#include "data_test_support.hpp"
#include "../../apps/viewer/src/presentation_constants.hpp"
#include "eawr/data/xml.hpp"

#include <array>
#include <cmath>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>

namespace eawr::tests::data_contracts {

eawr::testing::Checker local_checker;
int& failures = local_checker.failures;

void expect(const bool condition, const std::string_view message) {
    if (!condition) local_checker.check(condition, message, std::cerr, "FAIL: ");
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
            "<Shield_Flash_Duration>0.25</Shield_Flash_Duration>"
            "<GUI_Flash_Level>2</GUI_Flash_Level><GUI_Rapid_Flash_Duration>0.2</GUI_Rapid_Flash_Duration></GameConstants>";
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
        const auto order_flash = pc::load_order_flash(mounted.value());
        expect(order_flash.level == 2.0F && order_flash.pulse_seconds == 0.2, "effective XML order flash level and pulse");
        {   // WU-60/61: authored rapid linear fade, bounded amplitude, area-limited large hulls.
            const pc::OrderFlash stock{3.0F, 0.1};
            const auto at = [&](const double seconds) { return pc::order_flash_multiplier(stock, seconds); };
            expect(at(0) && std::abs(*at(0) - 2.0F) < 1e-4F, "stock comfort peak is twice the hull light");
            expect(at(0.025) && std::abs(*at(0.025) - 1.75F) < 1e-4F
                   && at(0.075) && std::abs(*at(0.075) - 1.25F) < 1e-4F, "linear fade through each rapid pulse");
            expect(at(0.1) == at(0) && at(0.2) == at(0), "authored tenth-second pulse restarts twice");
            expect(!at(0.31) && !at(4.0), "three rapid pulses end after 0.3 seconds");
            expect(!at(-0.01), "order flash has not started before its tick");
            expect(!pc::order_flash_multiplier(pc::OrderFlash{3.0F, 0.0}, 0.0), "zero pulse length shows nothing");
            expect(pc::order_flash_multiplier(pc::OrderFlash{3000.0F, 0.001}, 0.0) == at(0),
                   "mod strength cannot exceed presentation brightness bound");
            expect(!pc::order_flash_multiplier(pc::OrderFlash{3000.0F, 0.001}, 0.004),
                   "shorter authored durations retain their rapid cadence");
            expect(pc::order_flash_multiplier(pc::OrderFlash{1.5F, 1.2}, 0.6) == at(0.075),
                   "weaker and slower authored flashes are respected");
            const auto large = pc::order_flash_multiplier(stock, 0.0, 1000000.0);
            expect(large && std::abs(*large - 1.25F) < 1e-4F, "four times hull area gets one quarter added light");
            pc::OrderFlashGate gate;
            expect(gate.try_start(stock), "first target burst is admitted");
            gate.advance(0.3);
            expect(!gate.try_start(stock), "burst expiry alone does not rearm the target");
            gate.advance(0.99);
            expect(!gate.try_start(stock), "target stays quiet for a full real second after its burst");
            gate.advance(0.02);
            expect(gate.try_start(stock), "refused repeats do not extend the quiet interval");
            pc::OrderFlashGate another_target;
            expect(another_target.try_start(stock), "a different target is independently admitted");
            pc::OrderFlashGate slow_gate;
            const pc::OrderFlash slow{3.0F, 1.2};
            expect(slow_gate.try_start(slow), "longer authored burst starts");
            slow_gate.advance(2.0);
            expect(!slow_gate.try_start(slow), "a long authored burst cannot restart while still flashing");
            slow_gate.advance(2.61);
            expect(slow_gate.try_start(slow), "long authored burst also gets a full quiet second");

            // Twelve orders 0.2 seconds apart: the production gate must bound every
            // rolling real second to one burst, with steady frames while orders continue.
            pc::OrderFlashGate repeated_gate;
            std::optional<double> elapsed;
            std::array<bool, 240> active{};
            int starts = 0;
            int active_samples = 0;
            for (int frame = 0; frame < static_cast<int>(active.size()); ++frame) {
                if (frame <= 132 && frame % 12 == 0 && repeated_gate.try_start(stock)) {
                    elapsed = 0.0;
                    ++starts;
                }
                if (elapsed) {
                    active[static_cast<std::size_t>(frame)] = pc::order_flash_multiplier(stock, *elapsed).has_value();
                    if (active[static_cast<std::size_t>(frame)]) ++active_samples;
                    *elapsed += 1.0 / 60.0;
                }
                repeated_gate.advance(1.0 / 60.0);
            }
            expect(starts == 2 && active_samples >= 36 && active_samples <= 38,
                   "twelve rapid orders admit only two complete bursts");
            for (std::size_t begin = 0; begin + 60 <= active.size(); ++begin) {
                int window_samples = 0;
                for (std::size_t frame = begin; frame < begin + 60; ++frame) window_samples += active[frame] ? 1 : 0;
                expect(window_samples <= 19, "every rolling real second is bounded to one three-pulse burst");
            }
        }
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
        for (const auto invalid : {"-1", "nan", "inf", "1junk", "", "1,2"}) {
            const eawr::data::DocumentOverrides override({{"data/xml/gameconstants.xml", "GUI_Rapid_Flash_Duration", {invalid}},
                {"data/xml/gameconstants.xml", "GUI_Flash_Level", {invalid}}});
            const auto fallback = pc::load_order_flash(mounted.value());
            expect(fallback.level == 3.0F && fallback.pulse_seconds == 0.1, "invalid order flash values fall back");
        }
        const eawr::data::DocumentOverrides missing({{"data/xml/gameconstants.xml", "Laser_Kite_Z_Scale_Factor", {}}});
        expect(pc::load_lasers(mounted.value()).kite == 1.2F, "missing laser tag falls back");
    }
    std::filesystem::remove_all(scratch);
    run_contracts();
    tag_trace_contracts();
    retail_value_contracts();
    if (failures == 0) std::cout << "data contracts passed\n";
    else std::cerr << failures << " data contract test(s) failed\n";
    return failures == 0 ? 0 : 1;
}
