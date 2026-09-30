#include "viewer_host.hpp"

#include "ui/input_routing.hpp"
#include "ui/kit.hpp"
#include "ui/tactical_hud.hpp"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/godot.hpp>

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
    GDExtensionBinding::InitObject init(get_proc_address, library, initialization);
    init.register_initializer(initialize_eawr_viewer);
    init.register_terminator(uninitialize_eawr_viewer);
    init.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);
    return init.init();
}
