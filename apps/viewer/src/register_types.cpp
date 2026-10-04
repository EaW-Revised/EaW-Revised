#include "startup_trace.hpp"
#include "viewer_host.hpp"

#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include "ui/tactical_hud.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

// Keep Windows macros out of the UI and binding headers.
#ifdef _WIN32
#include <windows.h>
#endif

using namespace godot;

namespace {

void initialize_eawr_viewer(const ModuleInitializationLevel level) {
    if (level == MODULE_INITIALIZATION_LEVEL_SCENE) {
        GDREGISTER_CLASS(eawr::presentation::godot_backend::ViewerHost);
        eawr::presentation::godot_backend::register_ui_kit_classes();
        eawr::presentation::godot_backend::register_ui_input_classes();
        eawr::presentation::godot_backend::register_tactical_hud_classes();
    }
}

void uninitialize_eawr_viewer(const ModuleInitializationLevel) {}

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
