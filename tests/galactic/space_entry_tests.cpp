#include "space_entry_private.hpp"
#include "galactic_support.hpp"
#include <cstdlib>
#include <exception>

using namespace space_entry_test;

int case_galactic_space_entry_tests(int argc, char** argv) {
    if (argc > 1 && std::string_view(argv[1]) == "--installed-mp-difficulty") {
        try { installed(false, false, false, false, false, true); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
        return galactic_test::failures ? 1 : 0;
    }
    if (argc > 1 && std::string_view(argv[1]) == "--installed-orphan-rider") {
        try { installed(false, false, false, false, false, false, true); }
        catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
        return galactic_test::failures ? 1 : 0;
    }
#ifdef _WIN32
    _putenv_s("EAWR_VERIFY_GALACTIC_HASH", "1");
#else
    setenv("EAWR_VERIFY_GALACTIC_HASH", "1", 1);
#endif
    try { if (argc > 1 && std::string_view(argv[1]) == "--installed-layers") installed(false, false, false, false, true); else if (argc > 1 && std::string_view(argv[1]) == "--installed-commanders") installed(false, false, true); else if (argc > 1 && std::string_view(argv[1]) == "--installed-space-population") installed(false, true); else if (argc > 1 && std::string_view(argv[1]) == "--installed-superweapon") installed(false, false, false, true); else if (argc > 1 && std::string_view(argv[1]) == "--installed-garrison") installed(true); else if (argc > 1 && std::string_view(argv[1]) == "--installed") installed(); else { salvage_contracts(); script_manifest_contracts(); preparation_contracts(); nonparticipant_team_contracts(); scene_contracts(); reserve_order_contract(); orbital_owner_admission(); formation_contracts(); team_origin_contract(); formation_stage_contracts(); mixed_class_contracts(); garrison_contracts(); station_ship_spawn_contracts(); commander_contracts(); ground_passenger_contracts(); admitted_cycle(); ai_retreat_completion(); } }
    catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
    return galactic_test::failures ? 1 : 0;
}
