#include "lan_lobby_screen.hpp"
#include "lan_match.hpp"
#include "loading_screen.hpp"
#include "options_screen.hpp"
#include "startup_trace.hpp"
#include "crash_handler.hpp"
#include "crash_log_path.hpp"
#include "viewer_path.hpp"
#include "eawr/core/build_identity.hpp"
#include "gpu_memory_trace.hpp"
#include "viewer_host.hpp"
#include "galactic/radar_hud.hpp"
#include "galactic/fleet_drag.hpp"
#include "galactic/ground_hud.hpp"

#include "ui/credits_view.hpp"
#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include "ui/load_game_view.hpp"
#include "ui/message_box_view.hpp"
#include "ui/tactical_hud.hpp"
#include "ui/build_queue_dial.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/classes/project_settings.hpp>
#include <godot_cpp/classes/os.hpp>
#include <godot_cpp/variant/utility_functions.hpp>
#include <godot_cpp/godot.hpp>

// Keep Windows macros out of the UI and binding headers.
#ifdef _WIN32
#include <windows.h>
#endif

using namespace godot;

namespace {

void initialize_eawr_viewer(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        using namespace eawr::presentation::godot_backend;
#ifdef _WIN32
        auto* settings = ProjectSettings::get_singleton();
        const String default_log = settings->get_setting("debug/file_logging/log_path", String("user://logs/godot.log"));
        const auto arguments = OS::get_singleton()->get_cmdline_user_args();
        const std::string log = resolve_crash_log_path(default_log.utf8().get_data(),
            static_cast<std::size_t>(arguments.size()), [&](const std::size_t index) {
                return std::string(arguments[static_cast<int64_t>(index)].utf8().get_data());
            });
        const auto path = ViewerPath(std::string(settings->globalize_path(String::utf8(log.c_str())).utf8().get_data())).native();
        std::error_code error;
        std::filesystem::create_directories(path.parent_path(), error);
        if (!install_crash_handler(path.c_str(), eawr::core::build_identity().data())) {
            const auto fallback = ViewerPath(std::string(settings->globalize_path("user://logs/godot.log").utf8().get_data())).native();
            std::filesystem::create_directories(fallback.parent_path(), error);
            if (!install_crash_handler(fallback.c_str(), eawr::core::build_identity().data()))
                UtilityFunctions::printerr("EAWR crash handler could not initialize; crash files unavailable");
            else UtilityFunctions::printerr("EAWR crash files will use user://logs (custom log path unavailable)");
        }
        for (int64_t index = 0; index < arguments.size(); ++index)
            if (arguments[index] == "--eawr-test-crash") deliberate_test_crash();
#endif
        GDREGISTER_CLASS(eawr::presentation::godot_backend::ViewerHost);
        GDREGISTER_CLASS(eawr::presentation::godot_backend::EawrLoadingScreen);
        GDREGISTER_CLASS(eawr::presentation::godot_backend::EawrGalacticRadar);
        GDREGISTER_CLASS(eawr::presentation::godot_backend::EawrFleetDrag);
        GDREGISTER_CLASS(eawr::presentation::godot_backend::EawrGroundInput);
        GDREGISTER_CLASS(eawr::presentation::godot_backend::EawrOptionsScreen);
        eawr::presentation::godot_backend::register_ui_kit_classes();
        eawr::presentation::godot_backend::register_ui_input_classes();
        eawr::presentation::godot_backend::register_message_box_classes();
        eawr::presentation::godot_backend::register_lan_lobby_classes();
        eawr::presentation::godot_backend::register_load_game_classes();
        eawr::presentation::godot_backend::register_credits_classes();
        eawr::presentation::godot_backend::register_tactical_hud_classes();
        GDREGISTER_CLASS(eawr::presentation::godot_backend::EawrBuildQueueDial);
    }
}

void uninitialize_eawr_viewer(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        eawr::presentation::godot_backend::gpu_memory_trace.shutdown_journal();
        eawr::presentation::godot_backend::shutdown_lan_drains();
        eawr::presentation::godot_backend::uninstall_crash_handler();
    }
}

} // namespace

extern "C" GDExtensionBool GDE_EXPORT eawr_viewer_library_init(
    GDExtensionInterfaceGetProcAddress get_proc_address,
    GDExtensionClassLibraryPtr library,
    GDExtensionInitialization* initialization) {
#ifdef _WIN32
    FILETIME created{}, exited{}, kernel{}, user{}, now{};
    if (GetProcessTimes(GetCurrentProcess(), &created, &exited, &kernel, &user)) {
        GetSystemTimePreciseAsFileTime(&now);
        ULARGE_INTEGER creation{}, current{};
        creation.LowPart = created.dwLowDateTime; creation.HighPart = created.dwHighDateTime;
        current.LowPart = now.dwLowDateTime; current.HighPart = now.dwHighDateTime;
        if (current.QuadPart >= creation.QuadPart) {
            eawr::presentation::godot_backend::direct_start_origin = "process_start";
            using Trace = eawr::presentation::godot_backend::StartupTrace;
            eawr::presentation::godot_backend::process_start = Trace::Clock::now()
                - std::chrono::duration_cast<Trace::Clock::duration>(
                    std::chrono::duration<double>(static_cast<double>(current.QuadPart - creation.QuadPart) / 1.0e7));
        }
    }
#endif
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_eawr_viewer);
    init.register_terminator(uninitialize_eawr_viewer);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
